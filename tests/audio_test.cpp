#include "minunit.h"
#include "../src/audio_out/wasapi_audio_output.h"
#include <cstring>
#include <cmath>

struct AudioTest {
    static int RingAvail(WasapiAudioOutput& ao) { return ao.RingAvail(); }
    static int RingFree(WasapiAudioOutput& ao) { return ao.RingFree(); }
    static void RingWrite(WasapiAudioOutput& ao, const uint8_t* data, int size) {
        ao.RingWrite(data, size);
    }
    static float ReadSample(WasapiAudioOutput& ao, const uint8_t* p, int bits) {
        return ao.ReadSample(p, bits);
    }
    static void FillBuffer(WasapiAudioOutput& ao, BYTE* out, int out_frames) {
        ao.FillBuffer(out, out_frames);
    }

    // Ring setup
    static void InitRing(WasapiAudioOutput& ao, int size) {
        ao.ring_ = (uint8_t*)malloc(size);
        ao.ring_size_ = size;
        ao.ring_head_.store(0, std::memory_order_release);
        ao.ring_tail_.store(0, std::memory_order_release);
    }
    static void FreeRing(WasapiAudioOutput& ao) {
        free(ao.ring_);
        ao.ring_ = nullptr;
        ao.ring_size_ = 0;
    }

    // FillBuffer setup
    static void InitFillBuffer(WasapiAudioOutput& ao,
                               int in_rate, int out_rate,
                               int in_ch, int out_ch,
                               int in_bits) {
        InitRing(ao, 4*1024*1024);
        ao.in_rate_ = in_rate;
        ao.out_rate_ = out_rate;
        ao.in_channels_ = in_ch;
        ao.out_channels_ = out_ch;
        ao.in_bits_ = in_bits;
        ao.in_frame_bytes_ = in_ch * in_bits / 8;
        ao.out_frame_bytes_ = out_ch * 4;
        ao.buffer_frames_ = 256;
        ao.tmp_buf_size_ = ao.buffer_frames_ * 2 * ao.in_frame_bytes_;
        ao.tmp_buf_ = (uint8_t*)malloc(ao.tmp_buf_size_);
        ao.speed_ = 1.0;
        ao.resample_frac_ = 0;
    }
    static void CleanupFillBuffer(WasapiAudioOutput& ao) {
        free(ao.tmp_buf_);
        ao.tmp_buf_ = nullptr;
        FreeRing(ao);
    }
};

// ---------- Ring buffer tests ----------

static void test_ring_initial_state() {
    WasapiAudioOutput ao;
    AudioTest::InitRing(ao, 4*1024*1024);
    MU_CHECK_EQ(AudioTest::RingAvail(ao), 0);
    MU_CHECK_EQ(AudioTest::RingFree(ao), 4*1024*1024 - 1);
    AudioTest::FreeRing(ao);
}

static void test_ring_write_and_avail() {
    WasapiAudioOutput ao;
    AudioTest::InitRing(ao, 4*1024*1024);
    uint8_t data[100] = {1,2,3};
    AudioTest::RingWrite(ao, data, 100);
    MU_CHECK_EQ(AudioTest::RingAvail(ao), 100);
    MU_CHECK_EQ(AudioTest::RingFree(ao), 4*1024*1024 - 1 - 100);
    AudioTest::FreeRing(ao);
}

static void test_ring_wraparound() {
    WasapiAudioOutput ao;
    AudioTest::InitRing(ao, 256);
    // Fill ring to near-full
    uint8_t data1[200] = {0};
    AudioTest::RingWrite(ao, data1, 200);
    MU_CHECK_EQ(AudioTest::RingAvail(ao), 200);

    // Write wraps: head at 200, only 56 bytes left, 72 total → 56 fill end + 16 wrap to front
    uint8_t data2[72] = {0};
    AudioTest::RingWrite(ao, data2, 72);
    // head wraps to 16, tail still 0 → avail = (16-0) % 256 = 16
    MU_CHECK_EQ(AudioTest::RingAvail(ao), 16);
    AudioTest::FreeRing(ao);
}

// ---------- ReadSample tests ----------

static float read16(int16_t v) {
    WasapiAudioOutput ao;
    uint8_t bytes[2];
    bytes[0] = (uint8_t)(unsigned)v;
    bytes[1] = (uint8_t)((unsigned)v >> 8);
    return AudioTest::ReadSample(ao, bytes, 16);
}

static float read24(int32_t v) {
    WasapiAudioOutput ao;
    uint8_t bytes[3];
    bytes[0] = (uint8_t)(unsigned)v;
    bytes[1] = (uint8_t)((unsigned)v >> 8);
    bytes[2] = (uint8_t)((unsigned)v >> 16);
    return AudioTest::ReadSample(ao, bytes, 24);
}

static void test_readsample_16bit() {
    MU_CHECK_DBL(read16(0), 0.0f, 1e-7f);
    MU_CHECK_DBL(read16(32767), 32767/32768.0f, 1e-6f);
    MU_CHECK_DBL(read16(-32768), -1.0f, 1e-6f);
    MU_CHECK_DBL(read16(1), 1.0f/32768, 1e-7f);
}

static void test_readsample_24bit() {
    MU_CHECK_DBL(read24(0), 0.0f, 1e-7f);
    MU_CHECK_DBL(read24(8388607), 8388607/8388608.0f, 1e-6f);
    MU_CHECK_DBL(read24(-8388608), -1.0f, 1e-6f);
    MU_CHECK_DBL(read24(1), 1.0f/8388608, 1e-7f);
}

static void test_readsample_32bit() {
    WasapiAudioOutput ao;
    float v = 0.5f;
    uint8_t bytes[4];
    memcpy(bytes, &v, 4);
    float r = AudioTest::ReadSample(ao, bytes, 32);
    MU_CHECK_DBL(r, 0.5f, 1e-7f);

    v = -0.75f;
    memcpy(bytes, &v, 4);
    r = AudioTest::ReadSample(ao, bytes, 32);
    MU_CHECK_DBL(r, -0.75f, 1e-7f);
}

// ---------- FillBuffer tests ----------

static void test_fillbuffer_same_rate_no_resample() {
    WasapiAudioOutput ao;
    AudioTest::InitFillBuffer(ao, 44100, 44100, 2, 2, 16);

    // Need 5+ input frames to produce 4 output (in_needed = out_frames * ratio + 1)
    int16_t samples[12];  // 6 frames × 2 channels
    for (int i = 0; i < 6; i++) {
        samples[i*2]     = (int16_t)(10000 * sin(i * 3.14159 / 2));
        samples[i*2 + 1] = (int16_t)(10000 * cos(i * 3.14159 / 2));
    }
    AudioTest::RingWrite(ao, (uint8_t*)samples, 24);

    float out[8] = {0};
    AudioTest::FillBuffer(ao, (BYTE*)out, 4);

    // With same rate (ratio=1.0), output = input samples since frac=0 at integer positions
    for (int i = 0; i < 4; i++) {
        float expected_l = samples[i*2] / 32768.0f;
        float expected_r = samples[i*2+1] / 32768.0f;
        MU_CHECK_DBL(out[i*2],   expected_l, 3e-6f);
        MU_CHECK_DBL(out[i*2+1], expected_r, 3e-6f);
    }

    AudioTest::CleanupFillBuffer(ao);
}

static void test_fillbuffer_downsample() {
    WasapiAudioOutput ao;
    AudioTest::InitFillBuffer(ao, 48000, 44100, 1, 1, 16);

    int16_t in[8] = {100, 200, 300, 400, 500, 600, 700, 800};
    AudioTest::RingWrite(ao, (uint8_t*)in, 16);

    float out[4] = {0};
    AudioTest::FillBuffer(ao, (BYTE*)out, 4);

    MU_CHECK(fabs(out[0]) > 0 || fabs(out[1]) > 0 ||
             fabs(out[2]) > 0 || fabs(out[3]) > 0);

    AudioTest::CleanupFillBuffer(ao);
}

void audio_suite() {
    MU_RUN_TEST(test_ring_initial_state);
    MU_RUN_TEST(test_ring_write_and_avail);
    MU_RUN_TEST(test_ring_wraparound);
    MU_RUN_TEST(test_readsample_16bit);
    MU_RUN_TEST(test_readsample_24bit);
    MU_RUN_TEST(test_readsample_32bit);
    MU_RUN_TEST(test_fillbuffer_same_rate_no_resample);
    MU_RUN_TEST(test_fillbuffer_downsample);
}
