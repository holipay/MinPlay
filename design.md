# MiniPlayer 设计方案

---

## 1. 项目概述

### 1.1 项目定义

基于 Windows Media Foundation 的轻量级视频播放引擎，实现 M3U8 流媒体和本地 MP4 文件的播放，零外部依赖，自动硬件解码。

### 1.2 设计目标

```
目标一：零依赖部署
  exe 即运行，不携带任何第三方 DLL

目标二：自动硬件解码
  系统自动选择 DXVA2 / D3D11VA 解码器
  用户无需配置，开发者无需干预

目标三：架构清晰可扩展
  分层设计，各模块独立，可单独替换或扩展

目标四：极简交互
  一扇窗口显示画面，一套快捷键控制播放
  不追求 GUI 精美
```

### 1.3 功能范围

| 功能 | 支持 | 说明 |
|------|------|------|
| 本地 MP4 播放 | ✅ | H.264/H.265 + AAC |
| M3U8/HLS 流播放 | ✅ | 自适应码率由系统处理 |
| 暂停/继续 | ✅ | 空格键 |
| 快进/快退 | ✅ | 左右方向键，步进 10 秒 |
| 退出 | ✅ | ESC 键 |
| 硬件解码 | ✅ | MF 自动启用 |
| 播放列表 | ❌ | v2 考虑 |
| 字幕 | ❌ | v2 考虑 |
| 变速播放 | ❌ | v2 考虑 |
| 截图 | ❌ | v2 考虑 |

### 1.4 运行环境

```
操作系统：Windows 8.1 及以上（HLS 需要 Win 10）
编译器：  MSVC 2019+ 或 MinGW-w69
构建工具：CMake 3.15+
权限：    普通用户权限，无需管理员
```

---

## 2. 系统架构

### 2.1 分层架构图

```
┌─────────────────────────────────────────────────────┐
│                   用户交互层                          │
│         main.c — 窗口创建、消息循环、快捷键            │
├─────────────────────────────────────────────────────┤
│                   播放控制层                          │
│     core/player.c — 播放状态机、音视频同步引擎         │
├────────────┬──────────────────────┬─────────────────┤
│  视频输出层 │     媒体解码层        │   音频输出层     │
│ vo_gdi.c   │  mf_source.c        │  ao_waveout.c   │
│ GDI 渲染    │  Media Foundation   │  waveOut 输出    │
│            │  SourceReader        │                 │
├────────────┴──────────────────────┴─────────────────┤
│              Windows 系统层                           │
│  D3D11 硬解 │ HLS 协议栈 │ 音频驱动 │ 窗口管理器      │
└─────────────────────────────────────────────────────┘
```

### 2.2 线程模型

```
线程 1：主线程（UI 线程）
  ├── 创建窗口
  ├── 消息循环（GetMessage / DispatchMessage）
  ├── WM_PAINT → 调用 GDI 渲染当前帧
  └── WM_KEYDOWN → 发送命令到播放器

线程 2：解码线程（由 player.c 创建）
  ├── 循环调用 MFSourceReader::ReadSample
  ├── 视频帧 → 写入帧缓冲 → 通知主线程重绘
  ├── 音频帧 → 写入 waveOut 缓冲
  └── 音视频同步计算

Windows 系统线程：
  ├── waveOut 音频回调线程（音频设备驱动创建）
  └── MF 内部网络线程（HLS 分片下载）
```

```
时间线示意：
┌──────────────────────────────────────────────┐
│ 主线程:  消息循环 │ PAINT │ PAINT │ PAINT... │
├──────────────────────────────────────────────┤
│ 解码线程: READ→V│READ→A│READ→V│READ→A│...   │
├──────────────────────────────────────────────┤
│ waveOut:  播放│播放│播放│播放│播放│播放│...    │
├──────────────────────────────────────────────┤
│ MF网络:   下载ts1│下载ts2│下载ts3│...        │
└──────────────────────────────────────────────┘
```

### 2.3 数据流图

```
                        M3U8 URL 或 MP4 文件路径
                                │
                                ▼
                   ┌────────────────────────┐
                   │   Media Foundation     │
                   │   SourceReader         │
                   │                        │
                   │  ┌──────────────────┐  │
                   │  │ HLS 解复用器     │  │  ← M3U8 分片下载 + TS 解复用
                   │  │ MP4 解复用器     │  │  ← MP4 容器解析
                   │  │ 自动选择         │  │
                   │  └────────┬─────────┘  │
                   │           │             │
                   │  ┌────────▼─────────┐  │
                   │  │ 硬件解码器(MFT)  │  │  ← DXVA2 / D3D11VA
                   │  │ H.264 / H.265   │  │
                   │  └───┬─────────┬───┘  │
                   │      │         │       │
                   └──────┼─────────┼───────┘
                          │         │
              ┌───────────┘         └───────────┐
              ▼                                 ▼
     RGB32 视频帧                          PCM 音频帧
     (每帧 ~8MB)                          (每块 ~4KB)
              │                                 │
              ▼                                 ▼
    ┌──────────────┐                  ┌──────────────┐
    │  帧缓冲区     │                  │  waveOut     │
    │ (CRITICAL_SEC │                  │  音频缓冲     │
    │  保护)        │                  │  (8个缓冲轮转)│
    └──────┬───────┘                  └──────────────┘
           │
           ▼  InvalidateRect
    ┌──────────────┐
    │ GDI 渲染     │
    │ StretchDIBits│ → 屏幕显示
    └──────────────┘
```

---

## 3. 模块设计

### 3.1 模块清单

```
MiniPlayer/
├── main.c                  入口 + 窗口 + 消息循环
├── core/
│   ├── player.h            播放器接口定义
│   ├── player.c            播放控制、状态机、同步引擎
│   └── ringbuf.h           线程安全环形缓冲（可选）
├── media/
│   ├── media_source.h      统一媒体源接口
│   ├── media_source.c      MFSourceReader 封装
│   ├── hw_device.h         硬件解码设备管理接口
│   └── hw_device.c         D3D11 设备创建、MF 硬解关联
├── video_out/
│   ├── video_out.h         视频输出接口
│   └── vo_gdi.c            GDI StretchDIBits 实现
├── audio_out/
│   ├── audio_out.h         音频输出接口
│   └── ao_waveout.c        waveOut 实现
├── util/
│   ├── osd.h               屏幕信息叠加接口
│   ├── osd.c               帧率、时间、缓冲状态显示
│   └── log.h               调试日志宏
├── CMakeLists.txt          构建脚本
└── README.md               项目说明
```

### 3.2 模块 1：媒体解码层（media/）

#### 接口定义

```c
// media/media_source.h

#ifndef MEDIA_SOURCE_H
#define MEDIA_SOURCE_H

#include <stdint.h>

// ============================================================
// 帧数据结构
// ============================================================
typedef struct {
    uint8_t*    data;           // 像素数据 (RGB32) 或 PCM 数据
    int         size;           // data 字节数

    // 视频属性
    int         width;          // 像素宽
    int         height;         // 像素高
    int         stride;         // 行字节数

    // 音频属性
    int         sample_rate;    // 采样率 (Hz)
    int         channels;       // 声道数
    int         bits_per_sample;// 位深

    // 公共属性
    double      timestamp;      // 呈现时间戳 (秒)
    int         type;           // 0=音频, 1=视频
} MediaFrame;

// ============================================================
// 媒体源接口
// ============================================================
typedef struct MediaSource MediaSource;

typedef struct {
    int     width;
    int     height;
    double  fps;
} VideoInfo;

typedef struct {
    int     sample_rate;
    int     channels;
    int     bits_per_sample;
} AudioInfo;

// 生命周期
MediaSource*    media_open(const wchar_t* path_or_url);
void            media_close(MediaSource* src);

// 读取
int             media_read(MediaSource* src, MediaFrame* frame);
void            media_free_frame(MediaFrame* frame);

// 控制
int             media_seek(MediaSource* src, double seconds);
double          media_get_duration(MediaSource* src);
double          media_get_position(MediaSource* src);

// 信息查询
int             media_has_video(MediaSource* src);
int             media_has_audio(MediaSource* src);
int             media_get_video_info(MediaSource* src, VideoInfo* info);
int             media_get_audio_info(MediaSource* src, AudioInfo* info);

#endif // MEDIA_SOURCE_H
```

#### 实现要点

```c
// media/media_source.c — 关键实现片段

#include "media_source.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

struct MediaSource {
    IMFSourceReader* reader;
    int has_video, has_audio;
    VideoInfo vi;
    AudioInfo ai;
    double duration;
    double last_video_pts;
    double last_audio_pts;
};

MediaSource* media_open(const wchar_t* url) {
    MediaSource* src = calloc(1, sizeof(MediaSource));

    // 步骤 1：配置 SourceReader 属性
    IMFAttributes* attrs = NULL;
    MFCreateAttributes(&attrs, 4);
    attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    // 低质量模式（降低首帧延迟，HLS 场景有效）
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);

    // 步骤 2：创建 SourceReader（自动处理 MP4/HLS/MKV）
    HRESULT hr = MFCreateSourceReaderFromURL(url, attrs, &src->reader);
    attrs->Release();
    if (FAILED(hr)) { free(src); return NULL; }

    // 步骤 3：配置视频输出为 RGB32
    IMFMediaType* vmt = NULL;
    MFCreateMediaType(&vmt);
    vmt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    vmt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    hr = src->reader->SetCurrentMediaType(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, vmt);
    if (SUCCEEDED(hr)) {
        src->has_video = 1;
        // 读取视频尺寸和帧率
        IMFMediaType* native = NULL;
        src->reader->GetCurrentMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, &native);
        UINT32 w, h, num, den;
        MFGetAttributeSize(native, MF_MT_FRAME_SIZE, &w, &h);
        MFGetAttributeRatio(native, MF_MT_FRAME_RATE, &num, &den);
        src->vi.width = w;
        src->vi.height = h;
        src->vi.fps = den ? (double)num / den : 30.0;
        native->Release();
    }
    vmt->Release();

    // 步骤 4：配置音频输出为 PCM
    src->reader->SetStreamSelection(
        MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    IMFMediaType* amt = NULL;
    MFCreateMediaType(&amt);
    amt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    amt->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    amt->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
    amt->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    amt->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    hr = src->reader->SetCurrentMediaType(
        MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, amt);
    if (SUCCEEDED(hr)) {
        src->has_audio = 1;
        src->ai.sample_rate = 44100;
        src->ai.channels = 2;
        src->ai.bits_per_sample = 16;
    }
    amt->Release();

    // 步骤 5：读取时长
    PROPVARIANT dur;
    src->reader->GetPresentationDuration(&dur);
    if (dur.vt == VT_I8)
        src->duration = dur.hVal.QuadPart / 10000000.0;
    PropVariantClear(&dur);

    return src;
}
```

### 3.3 模块 2：播放控制层（core/）

#### 状态机设计

```
           media_open()
               │
               ▼
         ┌──────────┐
         │  STOPPED │ ←─────────┐
         └────┬─────┘           │
              │ player_play()   │ player_stop()
              ▼                 │
         ┌──────────┐           │
    ┌───→│ PLAYING  │───────────┘
    │    └────┬─────┘
    │         │ player_pause()
    │         ▼
    │    ┌──────────┐
    └────│ PAUSED   │
   resume└──────────┘

    任意状态 ──seek()──→ 刷新缓冲 → 恢复原状态
```

#### 接口定义

```c
// core/player.h

#ifndef PLAYER_H
#define PLAYER_H

#include <windows.h>

typedef struct Player Player;

typedef enum {
    STATE_STOPPED = 0,
    STATE_PLAYING = 1,
    STATE_PAUSED  = 2
} PlayerState;

// 生命周期
Player*         player_create(HWND hwnd);
int             player_open(Player* p, const wchar_t* url);
void            player_close(Player* p);
void            player_destroy(Player* p);

// 播放控制
void            player_play(Player* p);
void            player_pause_toggle(Player* p);
void            player_stop(Player* p);
void            player_seek(Player* p, double seconds);

// 状态查询
PlayerState     player_get_state(Player* p);
double          player_get_duration(Player* p);
double          player_get_position(Player* p);
int             player_has_video(Player* p);
int             player_has_audio(Player* p);

// 视频输出（主线程调用，用于 WM_PAINT）
void            player_paint(Player* p, HDC hdc, int w, int h);

#endif // PLAYER_H
```

#### 同步引擎设计

```c
// core/player.c — 同步引擎核心逻辑

// ===== 时钟管理 =====

typedef struct {
    double      audio_clock;        // 音频主时钟（由音频输出更新）
    double      video_clock;        // 最近一帧视频的 PTS
    double      external_clock;     // 系统墙钟（纯视频时备用）
    int64_t     clock_base;         // 墙钟基准点
    CRITICAL_SECTION lock;
} Clock;

static void clock_init(Clock* c) {
    InitializeCriticalSection(&c->lock);
    c->audio_clock = 0;
    c->video_clock = 0;
    c->external_clock = 0;
    c->clock_base = av_gettime_relative();  // 或用 QueryPerformanceCounter
}

static void clock_set_audio(Clock* c, double pts) {
    EnterCriticalSection(&c->lock);
    c->audio_clock = pts;
    LeaveCriticalSection(&c->lock);
}

static double clock_get_master(Clock* c, int has_audio) {
    EnterCriticalSection(&c->lock);
    double t = has_audio ? c->audio_clock : c->external_clock;
    LeaveCriticalSection(&c->lock);
    return t;
}

// ===== 同步逻辑 =====

// 返回值: 需要等待的毫秒数, -1=丢帧(视频太落后), 0=立即渲染
static int compute_video_delay(Clock* clk, double video_pts, int has_audio) {
    double master = clock_get_master(clk, has_audio);
    double diff = video_pts - master;

    if (diff < -0.1) {
        // 视频落后音频超过 100ms → 丢帧
        return -1;
    }
    else if (diff < -0.04) {
        // 视频落后 40~100ms → 不等待，立即渲染（追赶）
        return 0;
    }
    else if (diff > 0.5) {
        // 视频领先超过 500ms → 最多等 200ms（防止卡死）
        return 200;
    }
    else if (diff > 0.005) {
        // 正常领先 → 精确等待
        return (int)(diff * 1000);
    }
    else {
        // 几乎同步（±5ms）→ 立即渲染
        return 0;
    }
}

// ===== 解码主循环 =====

static DWORD WINAPI decode_thread(LPVOID arg) {
    Player* p = (Player*)arg;
    MediaFrame frame = {0};

    while (p->running) {
        // 等待非暂停状态
        while (p->state == STATE_PAUSED && p->running)
            Sleep(10);

        if (!p->running) break;

        // 读取一帧
        int ret = media_read(p->source, &frame);
        if (ret < 0) break;   // 文件结束或错误
        if (ret == 0) { Sleep(1); continue; }  // 暂无数据

        if (frame.type == 1) {
            // ---- 视频帧 ----
            int delay = compute_video_delay(
                &p->clock, frame.timestamp, p->has_audio);

            if (delay < 0) {
                // 丢帧：跳过此帧不渲染
                media_free_frame(&frame);
                continue;
            }

            if (delay > 0) {
                Sleep(delay);
            }

            // 更新帧缓冲
            EnterCriticalSection(&p->frame_lock);
            if (p->frame_buf && frame.size <= p->frame_buf_size) {
                memcpy(p->frame_buf, frame.data, frame.size);
                p->frame_ready = 1;
            }
            LeaveCriticalSection(&p->frame_lock);

            // 请求重绘
            InvalidateRect(p->hwnd, NULL, FALSE);
        }
        else {
            // ---- 音频帧 ----
            ao_write(p->ao, frame.data, frame.size);

            // 更新音频主时钟
            clock_set_audio(&p->clock, frame.timestamp);
        }

        media_free_frame(&frame);
    }

    return 0;
}
```

### 3.4 模块 3：视频输出层（video_out/）

#### 接口定义

```c
// video_out/video_out.h

#ifndef VIDEO_OUT_H
#define VIDEO_OUT_H

#include <windows.h>
#include <stdint.h>

typedef struct VideoOut VideoOut;

VideoOut*   vo_create(HWND hwnd, int width, int height);
void        vo_render(VideoOut* vo, const uint8_t* rgb32,
                       int src_w, int src_h);
void        vo_resize(VideoOut* vo, int win_w, int win_h);
void        vo_destroy(VideoOut* vo);

#endif
```

#### GDI 实现

```c
// video_out/vo_gdi.c

#include "video_out.h"

struct VideoOut {
    HWND hwnd;
    int  win_w, win_h;
};

VideoOut* vo_create(HWND hwnd, int w, int h) {
    VideoOut* vo = calloc(1, sizeof(VideoOut));
    vo->hwnd = hwnd;
    vo->win_w = w;
    vo->win_h = h;
    return vo;
}

void vo_render(VideoOut* vo, const uint8_t* rgb32, int src_w, int src_h) {
    HDC hdc = GetDC(vo->hwnd);

    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = src_w;
    bmi.bmiHeader.biHeight      = -src_h;  // 自上而下
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    StretchDIBits(hdc,
        0, 0, vo->win_w, vo->win_h,    // 目标矩形
        0, 0, src_w, src_h,             // 源矩形
        rgb32, &bmi, DIB_RGB_COLORS, SRCCOPY);

    ReleaseDC(vo->hwnd, hdc);
}

void vo_resize(VideoOut* vo, int w, int h) {
    vo->win_w = w;
    vo->win_h = h;
}

void vo_destroy(VideoOut* vo) {
    free(vo);
}
```

### 3.5 模块 4：音频输出层（audio_out/）

#### 接口定义

```c
// audio_out/audio_out.h

#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H

#include <stdint.h>

typedef struct AudioOut AudioOut;

AudioOut*   ao_create(int sample_rate, int channels, int bits);
void        ao_write(AudioOut* ao, const uint8_t* data, int size);
void        ao_pause(AudioOut* ao);
void        ao_resume(AudioOut* ao);
void        ao_reset(AudioOut* ao);     // 清空缓冲（seek 时调用）
void        ao_destroy(AudioOut* ao);

#endif
```

#### waveOut 实现

```c
// audio_out/ao_waveout.c

#include "audio_out.h"
#include <windows.h>

#define BUF_COUNT 8
#define BUF_SIZE  4096

struct AudioOut {
    HWAVEOUT handle;
    WAVEHDR  headers[BUF_COUNT];
    uint8_t  buffers[BUF_COUNT][BUF_SIZE];
    volatile int write_idx;
    CRITICAL_SECTION lock;
};

AudioOut* ao_create(int rate, int channels, int bits) {
    AudioOut* ao = calloc(1, sizeof(AudioOut));
    InitializeCriticalSection(&ao->lock);

    WAVEFORMATEX wfx = {0};
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = channels;
    wfx.nSamplesPerSec  = rate;
    wfx.wBitsPerSample  = bits;
    wfx.nBlockAlign     = channels * bits / 8;
    wfx.nAvgBytesPerSec = rate * wfx.nBlockAlign;

    waveOutOpen(&ao->handle, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);

    for (int i = 0; i < BUF_COUNT; i++) {
        ao->headers[i].lpData         = (LPSTR)ao->buffers[i];
        ao->headers[i].dwBufferLength = BUF_SIZE;
        ao->headers[i].dwFlags        = WHDR_DONE;
        waveOutPrepareHeader(ao->handle, &ao->headers[i], sizeof(WAVEHDR));
    }

    return ao;
}

void ao_write(AudioOut* ao, const uint8_t* data, int size) {
    EnterCriticalSection(&ao->lock);
    while (size > 0) {
        // 等待缓冲区空闲
        while (!(ao->headers[ao->write_idx].dwFlags & WHDR_DONE))
            Sleep(2);

        int copy = size < BUF_SIZE ? size : BUF_SIZE;
        memcpy(ao->buffers[ao->write_idx], data, copy);
        ao->headers[ao->write_idx].dwBufferLength = copy;
        ao->headers[ao->write_idx].dwFlags &= ~WHDR_DONE;
        waveOutWrite(ao->handle, &ao->headers[ao->write_idx], sizeof(WAVEHDR));

        data += copy;
        size -= copy;
        ao->write_idx = (ao->write_idx + 1) % BUF_COUNT;
    }
    LeaveCriticalSection(&ao->lock);
}

void ao_pause(AudioOut* ao) { waveOutPause(ao->handle); }
void ao_resume(AudioOut* ao) { waveOutRestart(ao->handle); }

void ao_reset(AudioOut* ao) {
    EnterCriticalSection(&ao->lock);
    waveOutReset(ao->handle);
    for (int i = 0; i < BUF_COUNT; i++) {
        ao->headers[i].dwFlags = WHDR_DONE;
        ao->headers[i].dwBufferLength = BUF_SIZE;
    }
    ao->write_idx = 0;
    LeaveCriticalSection(&ao->lock);
}

void ao_destroy(AudioOut* ao) {
    if (!ao) return;
    waveOutReset(ao->handle);
    for (int i = 0; i < BUF_COUNT; i++)
        waveOutUnprepareHeader(ao->handle, &ao->headers[i], sizeof(WAVEHDR));
    waveOutClose(ao->handle);
    DeleteCriticalSection(&ao->lock);
    free(ao);
}
```

### 3.6 模块 5：主入口（main.c）

```c
// main.c

#include <windows.h>
#include <stdio.h>
#include "core/player.h"
#include "util/log.h"

static Player*      g_player = NULL;
static HWND         g_hwnd   = NULL;
static volatile int g_running = 1;

// ============================================================
// 窗口过程
// ============================================================
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (g_player) {
                player_paint(g_player, hdc, rc.right, rc.bottom);
            } else {
                FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;   // 防止背景闪烁

        case WM_SIZE:
            if (g_player) {
                int w = LOWORD(lp), h = HIWORD(lp);
                // 通知播放器窗口尺寸变化
                // player_resize(g_player, w, h);
            }
            InvalidateRect(hwnd, NULL, FALSE);
            break;

        case WM_KEYDOWN:
            if (!g_player) break;
            switch (wp) {
                case VK_SPACE:
                    player_pause_toggle(g_player);
                    break;
                case VK_LEFT: {
                    double pos = player_get_position(g_player) - 10.0;
                    player_seek(g_player, pos < 0 ? 0 : pos);
                    break;
                }
                case VK_RIGHT: {
                    double pos = player_get_position(g_player) + 10.0;
                    double dur = player_get_duration(g_player);
                    player_seek(g_player, pos > dur ? dur : pos);
                    break;
                }
                case VK_ESCAPE:
                    DestroyWindow(hwnd);
                    break;
            }
            break;

        case WM_DESTROY:
            g_running = 0;
            PostQuitMessage(0);
            break;

        default:
            return DefWindowProc(hwnd, msg, wp, lp);
    }
    return 0;
}

// ============================================================
// 主函数
// ============================================================
int main(int argc, char* argv[])
{
    // 1. 解析参数
    wchar_t url[2048] = L"test.mp4";
    if (argc > 1)
        MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, url, 2048);

    // 2. 注册窗口类
    WNDCLASSEX wc = {sizeof(wc)};
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandle(NULL);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = TEXT("MiniPlayer");
    RegisterClassEx(&wc);

    // 3. 创建窗口
    RECT rc = {0, 0, 1280, 720};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowEx(0, TEXT("MiniPlayer"), TEXT("Mini Player"),
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, GetModuleHandle(NULL), NULL);

    // 4. 创建播放器并打开文件
    g_player = player_create(g_hwnd);
    if (!player_open(g_player, url)) {
        MessageBoxA(NULL, "无法打开媒体文件", "错误", MB_OK);
        return 1;
    }

    // 5. 开始播放
    player_play(g_player);
    LOG_INFO("播放中: %S", url);
    LOG_INFO("操作: 空格=暂停  ←→=快退快进  ESC=退出");

    // 6. 消息循环
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // 7. 清理
    player_close(g_player);
    player_destroy(g_player);
    return 0;
}
```

---

## 4. 线程安全设计

### 4.1 共享资源与保护策略

```
共享资源               写入方      读取方          保护方式
─────────────────────────────────────────────────────────
视频帧缓冲 g_frame_buf 解码线程    主线程(PAINT)   CRITICAL_SECTION
播放状态 g_state        主线程     解码线程        volatile int
音频时钟 audio_clock   解码线程    解码线程        CRITICAL_SECTION
窗口尺寸 g_win_w/h     主线程(WM_SIZE) 解码线程    volatile int
waveOut 缓冲区         解码线程    waveOut 线程    WHDR_DONE 标志轮询
```

### 4.2 线程间通信

```
主线程 → 解码线程:
  命令通过 volatile 变量传递
  player_pause_toggle → 设置 g_state = PAUSED
  player_seek         → 设置 g_seek_target, g_state = SEEKING

解码线程 → 主线程:
  InvalidateRect(hwnd) → 触发 WM_PAINT → 主线程读取帧缓冲

解码线程 → waveOut 线程:
  ao_write() 写入缓冲 → waveOutWrite → waveOut 自行消费
```

---

## 5. 构建系统

### 5.1 CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.15)
project(MiniPlayer C)

set(CMAKE_C_STANDARD 17)

# 源文件
add_executable(MiniPlayer WIN32
    src/main.c
    src/core/player.c
    src/media/media_source.c
    src/video_out/vo_gdi.c
    src/audio_out/ao_waveout.c
    src/util/osd.c
)

# Windows 系统库（零外部依赖）
target_link_libraries(MiniPlayer PRIVATE
    user32          # 窗口管理
    gdi32           # GDI 渲染
    ole32           # COM 初始化
    winmm           # waveOut 音频
    mf              # Media Foundation
    mfplat          # MF 平台
    mfreadwrite     # MF SourceReader
    mfuuid          # MF GUID
    uuid            # COM GUID
    shlwapi         # 辅助工具
)

# 附加 include 路径（如果需要）
target_include_directories(MiniPlayer PRIVATE src)

# MSVC 特定选项
if(MSVC)
    target_compile_options(MiniPlayer PRIVATE /W4 /utf-8)
    # 子系统设为 Console（保留 printf 输出）
    # 如果要去掉控制台窗口，改为 Windows
    set_target_properties(MiniPlayer PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:CONSOLE"
    )
endif()
```

### 5.2 编译命令

```powershell
# 方式一：CMake
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022"
cmake --build . --config Release

# 方式二：直接 MSVC
cl /O2 /W4 /utf-8 ^
    src/main.c src/core/player.c src/media/media_source.c ^
    src/video_out/vo_gdi.c src/audio_out/ao_waveout.c src/util/osd.c ^
    /Fe:MiniPlayer.exe ^
    /link user32.lib gdi32.lib ole32.lib winmm.lib ^
          mf.lib mfplat.lib mfreadwrite.lib mfuuid.lib uuid.lib shlwapi.lib

# 方式三：MinGW
gcc -O2 -Wall -o MiniPlayer.exe ^
    src/main.c src/core/player.c src/media/media_source.c ^
    src/video_out/vo_gdi.c src/audio_out/ao_waveout.c ^
    -lmf -lmfplat -lmfreadwrite -lole32 -lwinmm -luuid -lgdi32 -luser32
```

---

## 6. 部署方案

```
发布产物：
├── MiniPlayer.exe          ~100~150KB
└── (无其他文件)

系统要求：
├── Windows 10 1903+  ← 推荐（HLS + H.265 + 全部格式）
├── Windows 8.1       ← 最低（MP4 + H.264，无 HLS）
└── Windows 7         ← 不支持

用户使用：
  MiniPlayer.exe movie.mp4
  MiniPlayer.exe "https://example.com/live.m3u8"
  MiniPlayer.exe                    ← 默认打开 test.mp4
```

---

## 7. 版本规划

```
v0.1  基础播放（当前目标）
  ├── MP4 播放 (H.264 + AAC)
  ├── M3U8 播放
  ├── 硬件解码（自动）
  ├── GDI 渲染
  ├── waveOut 音频
  ├── 暂停/继续
  ├── 快进/快退
  └── 代码量: ~600 行

v0.2  体验优化
  ├── 进度条显示（OSD 叠加）
  ├── 总时长/当前时间显示
  ├── 音量控制（↑↓ 键）
  ├── 窗口缩放保持比例
  └── 代码量: +200 行

v0.3  高级功能
  ├── 拖放文件打开
  ├── 双击全屏
  ├── 文件关联（双击 MP4 直接用本程序打开）
  └── 代码量: +150 行

v0.4  性能优化
  ├── WASAPI 音频输出（替代 waveOut）
  ├── D3D11 渲染（替代 GDI，GPU 加速缩放）
  ├── NV12 纹理直接渲染（跳过 RGB 转换）
  └── 代码量: +400 行

v0.5  扩展功能
  ├── 播放列表
  ├── SRT 字幕加载
  ├── 截图（保存当前帧为 PNG）
  ├── 变速播放（1.5x / 2x）
  └── 代码量: +300 行
```

---

## 8. 风险与限制

```
风险项                     影响            缓解措施
─────────────────────────────────────────────────────────
MF HLS 不支持 Win7        部分用户无法使用  README 明确声明系统要求
冷门格式无法播放           用户困惑        错误提示明确告知格式不支持
部分 H.265 编码无法硬解    画面黑屏        检测硬解失败后提示用户
高延迟 HLS 流首帧慢        用户体验差      显示"加载中"提示
waveOut 延迟较高           音画可能不同步   v0.4 升级 WASAPI
GDI 渲染无硬件加速         CPU 占用略高     v0.4 升级 D3D11
```

---

## 9. 代码量估算

```
模块                   文件                   预估行数
──────────────────────────────────────────────────
主入口                 main.c                 120
播放引擎               core/player.c          180
媒体源                 media/media_source.c   200
视频输出               video_out/vo_gdi.c     60
音频输出               audio_out/ao_waveout.c 100
工具                   util/log.h + osd.c     80
头文件                 *.h (×6)               100
构建                   CMakeLists.txt         30
文档                   README.md              50
──────────────────────────────────────────────────
合计                                           920 行
```