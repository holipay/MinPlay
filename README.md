# MinPlay

A lightweight, native Windows media player built with C++, Media Foundation, WASAPI, and D3D11.

## Features

- **HLS live/VOD streaming** — custom `IMFByteStream` feeds TS segments to MF's TS demuxer
- **Audio-only mode** — `-a`/`--audio-only` flag plays audio from video files without video rendering
- **Hardware-accelerated video** — D3D11 NV12 GPU shader for YUV→RGB conversion
- **Low-latency audio** — WASAPI event-driven output with ring buffer and resampling
- **A/V sync** — frame-accurate synchronization against audio clock
- **OSD overlay** — time, duration, FPS, volume display via GDI on D3D11 back buffer
- **MPlayer-compatible controls** — keyboard shortcuts (Space, arrows, F11, etc.)
- **Mouse interaction** — click pause, double-click fullscreen, scroll volume
- **Fullscreen** — saves/restores window state, hides cursor
- **Process hardening** — heap termination on corruption, strict handle checks, AppInit DLL blocking
- **Security** — HTTP(S) only protocol whitelist, TLS 1.2+, certificate revocation checking

## Usage

```
MinPlay.exe video.mp4
MinPlay.exe -a video.mp4
MinPlay.exe --audio-only http://example.com/stream.m3u8
MinPlay.exe http://example.com/live.m3u8
```

## Build

Requires Visual Studio 2026 Community (vcvarsall).

```cmd
cmd /c build.bat
```

Output: `MinPlay.exe` in project root.

## Controls

| Input | Action |
|-------|--------|
| Space / P | Play / Pause |
| Left / Right | Seek ±10s |
| Up / Down | Seek ±60s |
| Ctrl+Left / Ctrl+Right | Seek ±600s |
| PageUp / PageDn | Seek ±60s |
| Home / End | Start / End |
| 0 / 9 | Volume ±10% |
| + / - | Volume ±1% |
| M | Mute |
| F / F11 | Toggle fullscreen |
| Q / ESC | Quit |
| Mouse click | Play / Pause |
| Mouse double-click | Toggle fullscreen |
| Scroll wheel | Volume ±5% |

## Architecture

```
src/
├── main.cpp              — Window + message loop + dispatch
├── core/
│   ├── player.cpp        — Play/pause/seek, A/V sync, frame display
│   └── source_reader_callback.cpp — IMFSourceReaderCallback impl
├── media/
│   └── media_source.cpp  — MFSourceReader setup, stream enumeration
├── audio_out/
│   └── wasapi_audio_output.cpp — WASAPI event-driven, ring buffer, resampling
├── video_out/
│   └── d3d11_video_output.cpp — D3D11 NV12 GPU shader, GDI OSD overlay
├── network/
│   └── hls_stream.cpp    — HLS m3u8 parser, WinHTTP downloader, IMFByteStream
├── sync/
│   └── sync_context.cpp  — A/V synchronization logic
└── util/
    ├── osd.cpp           — Time/fps overlay (GDI on D3D11 back buffer)
    └── yuv_convert.cpp   — YUY2/I420 → NV12 conversion
```

## Data Flow

```
MF callback → audio → ring buffer → WASAPI playback thread
            → video → player queue → main thread timer → D3D11 render → GPU YUV→RGB → GDI OSD → Present
```

## Testing

```cmd
cmd /c build_tests.bat
```

## License

See repository for license details.
