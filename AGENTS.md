# MinPlay — Agent Guide

## Build

**Always use `build.bat`** (or `cmake -B build && cmake --build build` — CMakeLists.txt is now correct).

```
cmd /c build.bat
```

Requires VS 2026 Community vcvarsall. Output: `MinPlay.exe` in project root.

Run: `.\MinPlay.exe D:\photo\build\test\call_me.mp4`

## Architecture

Single C++ (COM-style) project. No packages. Tests in `tests/` (29 tests, `build_tests.bat`). CI in `.github/workflows/build.yml`.

| Module | File | Role |
|--------|------|------|
| Entry | `src/main.cpp` | Window + message loop + dispatch |
| Core | `src/core/player.cpp` | Play/pause/seek, A/V sync, frame display |
| Callback | `src/core/source_reader_callback.cpp` | IMFSourceReaderCallback impl, routes samples |
| Media | `src/media/media_source.cpp` | MFSourceReader setup, stream enumeration |
| Audio out | `src/audio_out/wasapi_audio_output.cpp` | WASAPI event-driven, ring buffer, resampling |
| Video out | `src/video_out/d3d11_video_output.cpp` | D3D11 rendering with NV12 GPU shader |
| Network | `src/network/hls_stream.cpp` | HLS m3u8 parser, WinHTTP downloader, IMFByteStream |
| OSD | `src/util/osd.cpp` | Time/fps overlay |

Data flow: MF callback → audio direct to ring / video `player_process_video_frame` (both in MF thread) → `WM_TIMER` (main thread) → `player_video_tick` sync + `vo_render` D3D11 → GPU YUV→RGB via pixel shader.

## Critical gotchas

- **Build**: Only `build.bat` works. CMakeLists.txt is now correct (use `cmake -B build && cmake --build build`).
- **COM in C++**: Use `ComPtr<T>` (custom impl in `src/util/com_ptr.h`). Call `.reset()` or let destructor release. `operator&()` releases current pointer then returns address (safe only for empty ComPtrs passed to creation functions). `.ReleaseAndGetAddress()` does the same. Build with `/EHsc` (C++ exceptions on, SEH off).
- **IMFSample in OnReadSample — DO NOT Release**: MF owns the sample passed to `OnReadSample`. The C++ port added `pSample->Release()` at the end, which decremented MF's reference and caused `mfcore.dll` crash when MF tried to reuse the sample. C code never released pSample (C++ `ComPtr` would auto-release on function exit, so `.Release()` or letting the smart pointer release is also wrong unless you AddRef first).
- **WASAPI GUIDs**: Not in any import lib on SDK 10.0.28000.0. Must be defined as `static const` structs. See `wasapi_audio_output.cpp` header for values.
- **SourceReader callback re-request**: Audio re-request is throttled — only calls `ReadSample` when `ao_get_free > 256KB`. Video re-request uses `std::atomic<LONG> video_pending_` (< 2). `CheckAudio` timer (50ms) re-requests when ring is low.
- **Ring buffer SPSC**: `std::atomic<int> ring_head`/`ring_tail`, single producer (callback) + single consumer (playback thread). Safe on x86 without locks. `RingWrite` uses `memory_order_release` on head, `RingAvail` uses `memory_order_acquire` on both, `FillBuffer` uses `memory_order_release` on tail.
- **A/V sync**: No audio speed adjustment — video thread handles sync by dropping/delaying frames. Speed is always 1.0.
- **No Sleep in message loop**: Blocks message dispatch, freezes window.
- **Resample ratio**: `ratio = in_rate / out_rate * speed`. NOT `/ speed` — that inverts the direction and creates positive feedback.
- **Frame alignment in ao_write**: `to_write` is clamped to `in_frame_bytes` boundary to prevent ring corruption.
- **Video format negotiation**: Tries ARGB32 → RGB32(1) → RGB32(2) → NV12 → YUY2 → I420. NV12/RGB32 pass through to GPU directly; YUY2/I420 are converted to NV12 (data rearrangement, no color math) in `ProcessVideoFrame` (runs in MF callback thread).
- **Chinese filenames**: Uses `CommandLineToArgvW` (not `MultiByteToWideChar(CP_UTF8)`) because `argv` uses system codepage on Windows.
- **prebuf removed from WASAPI init**: MF source reader delivers small audio fragments (< prebuf threshold), causing WAV hang. Prebuf initialization was removed from `ao_wasapi.c`/`WasapiAudioOutput::Initialize`.
- **HLS via IMFByteStream**: Windows has no built-in HLS scheme handler (MF_E_UNSUPPORTED_BYTESTREAM_TYPE). Fallback: custom HlsByteStream (IMFByteStream) + MFCreateSourceReaderFromByteStream. TS segments downloaded via WinHTTP in background thread.
- **HLS pre-buffer**: First 3 TS segments are downloaded synchronously in `HlsManager::Open()` before source reader is created, to ensure MF has header data. Remaining segments downloaded by background thread.
- **Media type change**: HLS adaptive bitrate switching triggers MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED / CURRENTMEDIATYPECHANGED in OnReadSample. Handler re-sets output format and calls Player::OnVideoFormatChanged(). Resolution changes are filtered (>16px delta) to ignore stride-only changes (1080→1088).
- **Live stream**: HlsManager detects #EXT-X-ENDLIST absence → is_live_=true. Player::IsFinished() returns false for live, GetDuration() returns -1, Seek() is a no-op.
- **HlsByteStream async E_PENDING**: For live streams, `BeginRead` returns `E_PENDING` when no data is available (instead of completing with 0 bytes, which MF interprets as EOF). When `AddSegment` adds new data, it checks for a pending read and fulfills it by calling `CompleteAsync` (releases lock, invokes callback). `Close`/`Clear` use `CancelPendingRead` (no callback invocation — safe during shutdown).
- **HlsByteStream::Read must return S_OK not S_FALSE**: When `Read()` finds no data but no EOS marker, it must return `S_OK` with 0 bytes. Returning `S_FALSE` would make MF signal `MF_SOURCE_READERF_ENDOFSTREAM`, permanently stopping the pipeline. Only return `S_FALSE` when `has_eos_marker_` is set AND `read_pos_ >= total_bytes_`.
- **Live pipeline restart via needs_wake_**: If the pipeline stalls (EOF signaled by MF before new segments arrived), `AddSegment` sets `needs_wake_` flag. The player's `CheckAudio()` and `VideoTick()` timers check `source_->HasNewHlsData()` for live streams; if true, they flush the source reader (to clear MF's internal EOF state), reset EOF flags via `ResetAudioEof()`/`ResetVideoEof()`, and re-request samples.
- **HlsManager playlist reload**: `DownloadLoop()` calls `ReloadPlaylist()` for live streams after all known segments are consumed. `ReloadPlaylist` re-downloads the media m3u8, compares segment URLs against existing ones, and adds only genuinely new segments to the byte stream via `AddSegment`, which also fulfills any pending async read.
- **Network buffering**: Audio threshold is bps*5 (5s) for network vs bps/5 (200ms) for local files. Video queue fill target is 15 frames for network vs 1 for local. VQ_SIZE=32.

## Current state

HLS streaming works (live and VOD):
- m3u8 parsed (master → media playlist), WinHTTP downloads TS segments
- Custom HlsByteStream (IMFByteStream) feeds concatenated TS data to MF TS demuxer
- Pre-buffers 3 segments before source reader creation; background thread continues download
- Adaptive bitrate media type changes handled (resolution changes filtered for stride-only)
- Playlist reload for live streams: download loop calls `ReloadPlaylist()` when all known segments are consumed; new segments are parsed, downloaded, and added to the byte stream
- Async E_PENDING on read: live streams hold reads pending when no data, fulfilled by AddSegment — keeps MF pipeline alive without signaling EOF
- Live pipeline restart via needs_wake_: if pipeline stalls (EOF signaled), `AddSegment` sets flag; player timers reset EOF flags and re-request samples

WASAPI audio is working (MP4, WAV, HLS):
- Callback writes audio directly to ring buffer (bypasses main thread)
- Playback thread reads from ring → resamples → WASAPI output
- Audio re-request throttled to prevent burst delivery that drains ring
- Ring oscillates at healthy levels, no LOW warnings
- Network streams use 5s buffer threshold vs 200ms for local files

Video rendering is optimized:
- Main thread video timer (`SetTimer` at video FPS) replaces high-priority `timeSetEvent`
- CPU-side YUV→RGB conversion removed — NV12 data passed raw to D3D11 GPU shader
- YUY2/I420 → NV12 conversion is lightweight data rearrangement (no color math)
- `VideoTick` early-exits when no frames in queue
- Network streams target 15 buffered frames vs 1 for local (VQ_SIZE=32)

## Crash history

| Date | Issue | Root cause | Fix |
|------|-------|------------|-----|
| 2026-06 | C→C++ port MP4 crash | `pSample->Release()` in `OnReadSampleImpl` — MF owns the sample, dropping ref corrupts MF internal pool | Remove `pSample->Release()` (commit `d2b3f21` onward) |
| 2026-06 | C→C++ regression | Code between commits `efac72a` and `d2b3f21` compiles but no video output; commit `5ec7ed7` works | Not root-caused; likely format negotiation or stream selection issue |
| 2026-06 | WAV hang (C) | Source reader delivers small fragments (< prebuf bytes) causing deadlock | Remove prebuf from WASAPI init |
| 2026-06 | Program exits ~1s after start | Double-free in `VideoTick`: `free(frame_to_render.data)` + `VFrame::~VFrame()` frees same pointer. VFrame is a local with `~VFrame() { free(data); }`; explicit `free(frame_to_render.data)` leaves dangling pointer that destructor frees again → heap corruption | Remove explicit `free(frame_to_render.data)` — let destructor handle it |
