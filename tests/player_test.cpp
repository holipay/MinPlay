#include "minunit.h"
#include "../src/util/yuv_convert.h"
#include <cstdlib>

static void test_convert_yuy2_to_nv12_4x2() {
    uint8_t yuy2[] = {
        10, 128, 20, 129,  30, 130, 40, 131,
        50, 132, 60, 133,  70, 134, 80, 135,
    };
    uint8_t* nv12 = ConvertYUY2ToNV12(yuy2, 4, 2);
    MU_CHECK(nv12 != nullptr);

    MU_CHECK_EQ(nv12[0], 10);  MU_CHECK_EQ(nv12[1], 20);
    MU_CHECK_EQ(nv12[2], 30);  MU_CHECK_EQ(nv12[3], 40);
    MU_CHECK_EQ(nv12[4], 50);  MU_CHECK_EQ(nv12[5], 60);
    MU_CHECK_EQ(nv12[6], 70);  MU_CHECK_EQ(nv12[7], 80);

    uint8_t* uv = nv12 + 8;
    MU_CHECK_EQ(uv[0], 128);  MU_CHECK_EQ(uv[1], 129);
    MU_CHECK_EQ(uv[2], 130);  MU_CHECK_EQ(uv[3], 131);

    free(nv12);
}

static void test_convert_yuy2_to_nv12_odd_width_clamped() {
    uint8_t yuy2[12] = {0};
    uint8_t* nv12 = ConvertYUY2ToNV12(yuy2, 3, 2);
    MU_CHECK(nv12 != nullptr);
    free(nv12);
}

static void test_convert_yuy2_to_nv12_null_on_zero_width() {
    uint8_t yuy2[4] = {0};
    MU_CHECK(ConvertYUY2ToNV12(yuy2, 0, 2) == nullptr);
    MU_CHECK(ConvertYUY2ToNV12(yuy2, 1, 2) == nullptr);
}

static void test_convert_i420_to_nv12_4x2() {
    uint8_t i420[] = {
        10, 20, 30, 40, 50, 60, 70, 80,
        128, 130,
        129, 131,
    };
    uint8_t* nv12 = ConvertI420ToNV12(i420, 4, 2);
    MU_CHECK(nv12 != nullptr);

    MU_CHECK_EQ(nv12[0], 10);  MU_CHECK_EQ(nv12[1], 20);
    MU_CHECK_EQ(nv12[6], 70);  MU_CHECK_EQ(nv12[7], 80);

    uint8_t* uv = nv12 + 8;
    MU_CHECK_EQ(uv[0], 128);  MU_CHECK_EQ(uv[1], 129);
    MU_CHECK_EQ(uv[2], 130);  MU_CHECK_EQ(uv[3], 131);

    free(nv12);
}

static void test_convert_i420_to_nv12_6x4() {
    uint8_t i420[6*4 + (3*2)*2] = {0};
    for (int i = 0; i < 6*4; i++) i420[i] = (uint8_t)(i % 256);
    uint8_t* up = i420 + 6*4;
    for (int i = 0; i < 6; i++) up[i] = (uint8_t)(200 + i);
    uint8_t* vp = up + 6;
    for (int i = 0; i < 6; i++) vp[i] = (uint8_t)(100 + i);

    uint8_t* nv12 = ConvertI420ToNV12(i420, 6, 4);
    MU_CHECK(nv12 != nullptr);

    uint8_t* uv = nv12 + 6*4;
    MU_CHECK_EQ(uv[0], 200); MU_CHECK_EQ(uv[1], 100);
    MU_CHECK_EQ(uv[2], 201); MU_CHECK_EQ(uv[3], 101);
    MU_CHECK_EQ(uv[4], 202); MU_CHECK_EQ(uv[5], 102);
    MU_CHECK_EQ(uv[6], 203); MU_CHECK_EQ(uv[7], 103);
    MU_CHECK_EQ(uv[8], 204); MU_CHECK_EQ(uv[9], 104);

    free(nv12);
}

void player_suite() {
    MU_RUN_TEST(test_convert_yuy2_to_nv12_4x2);
    MU_RUN_TEST(test_convert_yuy2_to_nv12_odd_width_clamped);
    MU_RUN_TEST(test_convert_yuy2_to_nv12_null_on_zero_width);
    MU_RUN_TEST(test_convert_i420_to_nv12_4x2);
    MU_RUN_TEST(test_convert_i420_to_nv12_6x4);
}
