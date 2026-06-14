# MinPlay — Agent Guide

## Build

**Always use `build.bat`**. Do NOT use CMakeLists.txt — it is stale (missing `source_reader_callback.c`, uses `WIN32` subsystem with `main()` entry).

```
cmd /c build.bat
```

Requires VS 2026 Community vcvarsall. Output: `MinPlay.exe` in project root.

Run: `.\MinPlay.exe D:\photo\build\test\call_me.mp4`

## Architecture

Single C++ (COM-style) project. No packages, no tests, no CI.

| Module | File | Role |
|--------|------|------|
| Entry | `src/main.cpp` | Window + message loop + dispatch |
| Core | `src/core/player.cpp` | Play/pause/seek, A/V sync, frame display |
| Callback | `src/core/source_reader_callback.cpp` | IMFSourceReaderCallback impl, routes samples |
| Media | `src/media/media_source.cpp` | MFSourceReader setup, stream enumeration |
| Audio out | `src/audio_out/wasapi_audio_output.cpp` | WASAPI event-driven, ring buffer, resampling |
| Video out | `src/video_out/d3d11_video_output.cpp` | D3D11 rendering with NV12 GPU shader |
| OSD | `src/util/osd.cpp` | Time/fps overlay |

Data flow: MF callback → audio direct to ring / video `player_process_video_frame` (both in MF thread) → `WM_TIMER` (main thread) → `player_video_tick` sync + `vo_render` D3D11 → GPU YUV→RGB via pixel shader.

## Critical gotchas

- **Build**: Only `build.bat` works. CMakeLists.txt is wrong (uses C entry, wrong subsystem, stale file list).
- **COM in C++**: Use `ComPtr<T>` (from `mfapi.h` via `#include <mfobjects.h>`). Always call `.reset()` to release. Never call `.ReleaseAndGetAddressOf()` — that bypasses refcounting.
- **IMFSample in OnReadSample — DO NOT Release**: MF owns the sample passed to `OnReadSample`. The C++ port added `pSample->Release()` at the end, which decremented MF's reference and caused `mfcore.dll` crash when MF tried to reuse the sample. C code never released pSample (C++ `ComPtr` would auto-release on function exit, so `.Release()` or letting the smart pointer release is also wrong unless you AddRef first).
- **WASAPI GUIDs**: Not in any import lib on SDK 10.0.28000.0. Must be defined as `static const` structs. See `wasapi_audio_output.cpp` header for values.
- **SourceReader callback re-request**: Audio re-request is throttled — only calls `ReadSample` when `ao_get_free > 256KB`. Video re-request uses `video_pending_` counter (< 2). `CheckAudio` timer (50ms) re-requests when ring is low.
- **Ring buffer SPSC**: `volatile ring_head`/`ring_tail`, single producer (callback) + single consumer (playback thread). Safe on x86 without locks.
- **A/V sync**: No audio speed adjustment — video thread handles sync by dropping/delaying frames. Speed is always 1.0.
- **No Sleep in message loop**: Blocks message dispatch, freezes window.
- **Resample ratio**: `ratio = in_rate / out_rate * speed`. NOT `/ speed` — that inverts the direction and creates positive feedback.
- **Frame alignment in ao_write**: `to_write` is clamped to `in_frame_bytes` boundary to prevent ring corruption.
- **Video format negotiation**: Tries ARGB32 → RGB32(1) → RGB32(2) → NV12 → YUY2 → I420. NV12/RGB32 pass through to GPU directly; YUY2/I420 are converted to NV12 (data rearrangement, no color math) in `ProcessVideoFrame` (runs in MF callback thread).
- **Chinese filenames**: Uses `CommandLineToArgvW` (not `MultiByteToWideChar(CP_UTF8)`) because `argv` uses system codepage on Windows.
- **prebuf removed from WASAPI init**: MF source reader delivers small audio fragments (< prebuf threshold), causing WAV hang. Prebuf initialization was removed from `ao_wasapi.c`/`WasapiAudioOutput::Initialize`.

## Current state

WASAPI audio is working (MP4, WAV):
- Callback writes audio directly to ring buffer (bypasses main thread)
- Playback thread reads from ring → resamples → WASAPI output
- Audio re-request throttled to prevent burst delivery that drains ring
- Ring oscillates at healthy levels, no LOW warnings

Video rendering is optimized:
- Main thread video timer (`SetTimer` at video FPS) replaces high-priority `timeSetEvent`
- CPU-side YUV→RGB conversion removed — NV12 data passed raw to D3D11 GPU shader
- YUY2/I420 → NV12 conversion is lightweight data rearrangement (no color math)
- `VideoTick` early-exits when no frames in queue

## Crash history

| Date | Issue | Root cause | Fix |
|------|-------|------------|-----|
| 2026-06 | C→C++ port MP4 crash | `pSample->Release()` in `OnReadSampleImpl` — MF owns the sample, dropping ref corrupts MF internal pool | Remove `pSample->Release()` (commit `d2b3f21` onward) |
| 2026-06 | C→C++ regression | Code between commits `efac72a` and `d2b3f21` compiles but no video output; commit `5ec7ed7` works | Not root-caused; likely format negotiation or stream selection issue |
| 2026-06 | WAV hang (C) | Source reader delivers small fragments (< prebuf bytes) causing deadlock | Remove prebuf from WASAPI init |
| 2026-06 | Program exits ~1s after start | Double-free in `VideoTick`: `free(frame_to_render.data)` + `VFrame::~VFrame()` frees same pointer. VFrame is a local with `~VFrame() { free(data); }`; explicit `free(frame_to_render.data)` leaves dangling pointer that destructor frees again → heap corruption | Remove explicit `free(frame_to_render.data)` — let destructor handle it |
