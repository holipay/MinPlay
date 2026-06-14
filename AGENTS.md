# MiniPlayer — Agent Guide

## Build

**Always use `build.bat`**. Do NOT use CMakeLists.txt — it is stale (missing `source_reader_callback.c`, uses `WIN32` subsystem with `main()` entry).

```
cmd /c build.bat
```

Requires VS 2026 Community vcvarsall. Output: `MiniPlayer.exe` in project root.

Run: `.\MiniPlayer.exe D:\photo\build\test\call_me.mp4`

## Architecture

Single C project. No packages, no tests, no CI.

| Module | File | Role |
|--------|------|------|
| Entry | `src/main.c` | Window + message loop + dispatch |
| Core | `src/core/player.c` | Play/pause/seek, A/V sync, frame display |
| Callback | `src/core/source_reader_callback.c` | IMFSourceReaderCallback impl, routes samples |
| Media | `src/media/media_source.c` | MFSourceReader setup, stream enumeration |
| Audio out | `src/audio_out/ao_wasapi.c` | WASAPI event-driven, ring buffer, resampling |
| Video out | `src/video_out/vo_d3d11.c` | D3D11 rendering with NV12 GPU shader |
| OSD | `src/util/osd.c` | Time/fps overlay |

Data flow: MF callback → audio direct to ring / video `player_process_video_frame` (both in MF thread) → `WM_TIMER` (main thread) → `player_video_tick` sync + `vo_render` D3D11 → GPU YUV→RGB via pixel shader.

## Critical gotchas

- **Build**: Only `build.bat` works. CMakeLists.txt is wrong.
- **COM in C**: Always `ptr->lpVtbl->Method(ptr, args)`. `COBJMACROS` breaks with WASAPI `REFERENCE_TIME` params.
- **WASAPI GUIDs**: Not in any import lib on SDK 10.0.28000.0. Must be defined as `static const` structs. See `ao_wasapi.c` header for values.
- **SourceReader callback re-request**: Audio re-request is throttled — only calls `ReadSample` when `ao_get_free > 256KB`. Video re-request is unconditional. `player_check_audio` timer (50ms)补充请求 when ring is low.
- **IMFSample refcounting**: `OnReadSample` delivers a sample the callback owns. Audio/video both process synchronously in the callback (direct ring write / `player_process_video_frame`), so no AddRef/PostMessage needed.
- **Ring buffer SPSC**: `volatile ring_head`/`ring_tail`, single producer (callback) + single consumer (playback thread). Safe on x86 without locks.
- **A/V sync**: No audio speed adjustment — video thread handles sync by dropping/delaying frames. Speed is always 1.0.
- **No Sleep in message loop**: Blocks message dispatch, freezes window.
- **Resample ratio**: `ratio = in_rate / out_rate * speed`. NOT `/ speed` — that inverts the direction and creates positive feedback.
- **Frame alignment in ao_write**: `to_write` is clamped to `in_frame_bytes` boundary to prevent ring corruption.
- **Video format negotiation**: Tries ARGB32 → RGB32 → NV12 → YUY2 → I420. NV12/RGB32 pass through to GPU directly; YUY2/I420 are converted to NV12 (data rearrangement, no color math) in `player_process_video_frame` (runs in MF callback thread).
- **Chinese filenames**: Uses `CommandLineToArgvW` (not `MultiByteToWideChar(CP_UTF8)`) because `argv` uses system codepage on Windows.

## Current state

WASAPI audio is working:
- Callback writes audio directly to ring buffer (bypasses main thread)
- Playback thread reads from ring → resamples → WASAPI output
- Audio re-request throttled to prevent burst delivery that drains ring
- Ring oscillates at healthy levels, no LOW warnings
- A/V sync handled by video frame dropping (no audio speed adjustment)

Video rendering is optimized:
- Main thread video timer (`SetTimer` at video FPS) replaces high-priority `timeSetEvent`
- CPU-side YUV→RGB conversion removed — NV12 data passed raw to D3D11 GPU shader
- YUY2/I420 → NV12 conversion is lightweight data rearrangement (no color math)
- `player_video_tick` early-exits when no frames in queue
