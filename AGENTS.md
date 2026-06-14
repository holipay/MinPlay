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
| Video out | `src/video_out/vo_gdi.c` | GDI StretchDIBits rendering |
| OSD | `src/util/osd.c` | Time/fps overlay |

Data flow: MF callback → `ao_write` direct to ring (audio) / `PostMessage` (video) → ring buffer → playback thread → WASAPI.

## Critical gotchas

- **Build**: Only `build.bat` works. CMakeLists.txt is wrong.
- **COM in C**: Always `ptr->lpVtbl->Method(ptr, args)`. `COBJMACROS` breaks with WASAPI `REFERENCE_TIME` params.
- **WASAPI GUIDs**: Not in any import lib on SDK 10.0.28000.0. Must be defined as `static const` structs. See `ao_wasapi.c` header for values.
- **SourceReader callback re-request**: Audio re-request is throttled — only calls `ReadSample` when `ao_get_free > 256KB`. Video re-request is unconditional. `player_check_audio` timer (50ms)补充请求 when ring is low.
- **IMFSample refcounting**: `OnReadSample` delivers a sample the callback owns. Must `AddRef` before `PostMessage`; must `Release` on PostMessage failure. Direct ring write skips PostMessage.
- **Ring buffer SPSC**: `volatile ring_head`/`ring_tail`, single producer (callback) + single consumer (playback thread). Safe on x86 without locks.
- **A/V sync**: No audio speed adjustment — video thread handles sync by dropping/delaying frames. Speed is always 1.0.
- **No Sleep in message loop**: Blocks message dispatch, freezes window.
- **Resample ratio**: `ratio = in_rate / out_rate * speed`. NOT `/ speed` — that inverts the direction and creates positive feedback.
- **Frame alignment in ao_write**: `to_write` is clamped to `in_frame_bytes` boundary to prevent ring corruption.
- **Video format negotiation**: Tries ARGB32 → RGB32 → NV12 → YUY2 → I420. Non-RGB formats are converted to RGB32 in `player_process_video_frame`.
- **Chinese filenames**: Uses `CommandLineToArgvW` (not `MultiByteToWideChar(CP_UTF8)`) because `argv` uses system codepage on Windows.

## Current state

WASAPI audio is working:
- Callback writes audio directly to ring buffer (bypasses main thread)
- Playback thread reads from ring → resamples → WASAPI output
- Audio re-request throttled to prevent burst delivery that drains ring
- Ring oscillates at healthy levels, no LOW warnings
- A/V sync handled by video frame dropping (no audio speed adjustment)
