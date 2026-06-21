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
| Video out | `src/video_out/d3d11_video_output.cpp` | D3D11 rendering with NV12 GPU shader, GDI OSD overlay |
| Network | `src/network/hls_stream.cpp` | HLS m3u8 parser, WinHTTP downloader, IMFByteStream |
| OSD | `src/util/osd.cpp` | Time/fps overlay (drawn via GDI on D3D11 back buffer) |

Data flow: MF callback → audio direct to ring / video `player_process_video_frame` (both in MF thread) → `WM_TIMER` (main thread) → `player_video_tick` sync + `vo_render` D3D11 → GPU YUV→RGB via pixel shader → GDI OSD overlay on back buffer → Present.

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
- **Video format negotiation**: Tries NV12 → I420 → YUY2 → ARGB32 → RGB32 (prefers native GPU-friendly NV12). NV12/RGB32 pass through to GPU directly; YUY2/I420 are converted to NV12 (data rearrangement, no color math) in `ProcessVideoFrame` (runs in MF callback thread). If all formats fail, `has_video_` stays false and no video is rendered.
- **Chinese filenames**: Uses `CommandLineToArgvW` (not `MultiByteToWideChar(CP_UTF8)`) because `argv` uses system codepage on Windows.
- **prebuf removed from WASAPI init**: MF source reader delivers small audio fragments (< prebuf threshold), causing WAV hang. Prebuf initialization was removed from `ao_wasapi.c`/`WasapiAudioOutput::Initialize`.
- **HLS via custom IMFByteStream**: Windows has no built-in HLS scheme handler (MF_E_UNSUPPORTED_BYTESTREAM_TYPE). Custom HlsByteStream (IMFByteStream) concatenates TS segments and feeds MF's TS demuxer via `MFCreateSourceReaderFromByteStream`. Key requirement: `BeginRead` must **never return `E_PENDING`** — always complete via `MFInvokeCallback` immediately. MF's TS demuxer blocks forever during initial probing if `BeginRead` returns `E_PENDING`.
- **HLS byte stream must never return 0 bytes**: MF source reader sets an **immutable internal EOF flag** when bytestream returns 0 bytes. Once set, all subsequent `ReadSample` calls return EOF without calling `BeginRead`. The EOF flag cannot be cleared by `Stop()+StartReading()`, `ReadSample`, or likely `SetCurrentPosition`. Only recreating the source reader clears it. **Fix**: HlsByteStream uses a `data_event_` (Windows event) signaled by `AddSegment()`. `Read`/`BeginRead` wait on this event instead of polling with Sleep, so they block efficiently until data arrives and never return 0 bytes prematurely.
- **HLS live pipeline restart via RecreateReader**: When pipeline stalls (no video frames for 3+ seconds), `TryRestartLivePipeline()` triggers `FlushAndRestart()` which calls `MediaSource::RecreateReader()`. This creates a new MFSourceReader from the existing HlsByteStream (position 0). The old source reader is released AFTER the new one is created (avoiding dangling pointer). Byte stream data is preserved — `HlsByteStream::Close()` is a no-op because MF calls Close() during source reader destruction, which would clear all segment data. Data cleanup is handled by `HlsManager::Close()` → `Clear()`. `AddRef()` on byte stream in RecreateReader prevents destruction during recreation window.
- **HlsByteStream::Abort() for shutdown**: `Close()` is a no-op (to avoid MF clearing data during RecreateReader). But `BeginRead`/`Read` can block up to 60s on `data_event_` when download thread stops. `Abort()` sets `closed_=true` + signals `data_event_`, unblocking pending reads. Called in `HlsManager::Close()` before `reader_.reset()` to prevent MF source reader release from hanging.
- **HLS live stream EOF handling**: When EOF is signaled, callback attempts `SetCurrentPosition(0)` + `ReadSample` to clear MF's internal state. Combined with 2s wait in byte stream, this prevents EOF from being set in the first place.
- **HLS pre-buffer**: First 3 TS segments are downloaded synchronously in `HlsManager::Open()` before source reader is created, to ensure MF has header data. Remaining segments downloaded by background thread.
- **Media type change**: HLS adaptive bitrate switching triggers MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED / CURRENTMEDIATYPECHANGED in OnReadSample. Handler re-sets output format and calls Player::OnVideoFormatChanged(). Resolution changes are filtered (>16px delta) to ignore stride-only changes (1080→1088).
- **Live stream**: HlsManager detects #EXT-X-ENDLIST absence → is_live_=true. Player::IsFinished() returns false for live, GetDuration() returns -1, Seek() is a no-op.
- **HlsByteStream blocks MFCreateSourceReaderFromByteStream**: MF's TS demuxer blocks forever during initial probing if `BeginRead` returns `E_PENDING`. The demuxer calls `BeginRead` to probe the container, and if no data is available (after pre-buffer is consumed), `E_PENDING` tells MF "I'll call you back" — but during source reader creation, MF waits synchronously and never retries. Fix: `BeginRead` **always completes via `MFInvokeCallback`** immediately, never returns `E_PENDING`. For 0 bytes without EOS, use `S_OK` (not `S_FALSE`) so MF does not prematurely signal end-of-stream during playback.
- **HlsByteStream::Read must return S_OK not S_FALSE**: When `Read()` finds no data but no EOS marker, it must return `S_OK` with 0 bytes. Returning `S_FALSE` would make MF signal `MF_SOURCE_READERF_ENDOFSTREAM`, permanently stopping the pipeline. Only return `S_FALSE` when `has_eos_marker_` is set AND `read_pos_ >= total_bytes_`.
- **HLS live pipeline restart via needs_wake_**: If the pipeline stalls (EOF signaled by MF before new segments arrived), `AddSegment` sets `needs_wake_` flag. The player's `CheckAudio()` and `VideoTick()` timers check `source_->HasNewHlsData()` for live streams; if true, they flush the source reader (to clear MF's internal EOF state), reset EOF flags via `ResetAudioEof()`/`ResetVideoEof()`, and re-request samples.
- **Live EOF immediate restart**: When MF's TS demuxer signals EOF (after consuming all buffered data), the callback sets EOF flags and calls `Player::NotifyLiveEof()` which immediately triggers `FlushAndRestart()` if new data is available — no 3-second stall wait needed. `FlushAndRestart()` clears the video queue and resets audio buffer to prevent stale frame rendering.
- **HlsManager playlist reload**: `DownloadLoop()` calls `ReloadPlaylist()` for live streams after all known segments are consumed. `ReloadPlaylist` re-downloads the media m3u8, compares segment URLs against existing ones, and adds only genuinely new segments to the byte stream via `AddSegment`.
- **Network buffering**: Audio threshold is bps*5 (5s) for network vs bps/5 (200ms) for local files. Video queue fill target is 15 frames for network vs 1 for local. VQ_SIZE=32.
- **OSD via D3D11 back buffer GDI interop**: OSD text is drawn via GDI on the D3D11 back buffer before `Present()`. Uses `IDXGISurface1::GetDC()` on the swap chain back buffer. Requires `DXGI_SWAP_EFFECT_DISCARD` (not FLIP_DISCARD — FLIP doesn't support GDI interop). The overlay callback is set via `D3D11VideoOutput::SetOverlay()`, called from `Player::OpenAsync()` after D3D11 init.

## Current state

HLS streaming works (live and VOD):
- m3u8 parsed (master → media playlist), WinHTTP downloads TS segments
- Custom HlsByteStream (IMFByteStream) feeds concatenated TS data to MF TS demuxer
- Pre-buffers 1 segment before source reader creation; background thread continues download
- Adaptive bitrate media type changes handled (resolution changes filtered for stride-only)
- Playlist reload for live streams: download loop calls `ReloadPlaylist()` when all known segments are consumed; new segments are parsed, downloaded, and added to the byte stream
- `BeginRead` always completes immediately via callback (never `E_PENDING`) — avoids MF TS demuxer hang during initial probing
- HlsByteStream uses `data_event_` (Windows event) for blocking reads — `Read`/`BeginRead` wait on the event instead of polling, preventing MF from ever seeing 0 bytes and signaling premature EOF
- EOF handler: for live streams, does NOT set `video_eof_`/`audio_eof_` flags and re-requests immediately (no 50ms sleep), keeping the pipeline flowing without stutter
- `needs_wake_` mechanism for live pipeline restart: if pipeline stalls, `AddSegment` sets flag; player timers check `HasNewHlsData()` and flush/reset as needed
- Stall-based restart: 3s stall timeout, 5s cooldown. `RecreateReader` calls `DiscardConsumedData()` on the byte stream before creating a new source reader — removes all segments fully consumed by the old reader, shifts remaining offsets to start from 0, and resets `read_pos_` to 0. New reader only sees unread data (at most one segment of overlap with old data).

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
- OSD overlay drawn via GDI on D3D11 back buffer (requires `DXGI_SWAP_EFFECT_DISCARD` for GDI interop)

User interaction (v0.1.1):
- Mouse: single-click pause/play, double-click fullscreen, scroll wheel volume ±5%
- Keyboard: Space pause, Left/Right seek ±10s, Up/Down volume ±5%, M mute, F11 fullscreen, ESC exit fullscreen/quit
- Keyboard (MPlayer-compatible): f fullscreen, q quit, PgUp/PgDn seek ±60s, Home/End start/end, Ctrl+Left/Right seek ±600s, 0/9 volume ±10%, +/- volume ±1%
- Fullscreen: saves/restores window style and position, hides cursor
- Volume: 0-100% range, applied in WASAPI FillBuffer after resampling
- OSD: shows volume bar when not at 100%, shows MUTE indicator

## Security

### Input validation
- Protocol whitelist in `main.cpp:IsSchemeAllowed()` — only `http://`, `https://`, and local files accepted. `rtsp://`, `file://`, etc. rejected at startup.
- URL length bounded by `wcsncpy_s(url, 2048, ...)`. Embedded nulls prevented by `wcsnlen_s`.

### WinHTTP security
- TLS restricted to 1.2 and 1.3 (`WINHTTP_OPTION_SECURE_PROTOCOLS`) — no SSLv2/3, no TLS 1.0/1.1.
- Certificate revocation checking enabled (`WINHTTP_ENABLE_SSL_REVOCATION`).
- Default certificate validation (WinHTTP rejects untrusted/mismatched certs by default — no ignore flags set).

### Process hardening
- `HeapEnableTerminationOnCorruption` — fail-fast on heap corruption.
- `ProcessExtensionPointDisablePolicy` — blocks AppInit DLL injection.
- `ProcessStrictHandleCheckPolicy` — raises exception on `CloseHandle(INVALID_HANDLE_VALUE)`.

### Limitations (not implemented)
- **No AppContainer / low integrity level**: Would break GPU (D3D11 hardware decode), WASAPI audio, and file system access. Mitigation policies above provide partial isolation.
- **No signed binary**: ProcessSignaturePolicy would block loading MSVC runtime / MF DLLs.
- **No dynamic code restriction**: ProcessDynamicCodePolicy would break D3DCompile runtime shader compilation.
- **No network sandbox**: WinHTTP uses default proxy settings; no loopback restriction.

## Crash history

| Date | Issue | Root cause | Fix |
|------|-------|------------|-----|
| 2026-06 | C→C++ port MP4 crash | `pSample->Release()` in `OnReadSampleImpl` — MF owns the sample, dropping ref corrupts MF internal pool | Remove `pSample->Release()` (commit `d2b3f21` onward) |
| 2026-06 | C→C++ regression | Code between commits `efac72a` and `d2b3f21` compiles but no video output; commit `5ec7ed7` works | Not root-caused; likely format negotiation or stream selection issue |
| 2026-06 | WAV hang (C) | Source reader delivers small fragments (< prebuf bytes) causing deadlock | Remove prebuf from WASAPI init |
| 2026-06 | HLS live freeze | MF source reader caches EOF when bytestream returns 0 bytes; EOF flag is immutable. Stop()+StartReading() cannot clear it. | HlsByteStream uses `data_event_` (Windows event) signaled by `AddSegment()`. `Read`/`BeginRead` wait on this event instead of polling with Sleep, so they block efficiently until data arrives and never return 0 bytes prematurely. |
| 2026-06 | HLS live RecreateReader GetLength->0 | MF calls `HlsByteStream::Close()` during old source reader release, clearing all segment data | `Close()` now a no-op; data cleanup handled by `HlsManager::Close()` → `Clear()` |
| 2026-06 | HLS live replay old video | RecreateReader creates new source reader from byte stream at position 0, replaying all old segments before reaching new data | `DiscardConsumedData()` removes segments before `read_pos_`, shifts offsets to 0 |
| 2026-06 | HLS live stutter + fast playback | EOF storm (MF TS demuxer signals EOF after consuming buffered data), 50ms Sleep in EOF handler, video queue not cleared on restart | Live EOF sets EOF flags + triggers immediate `NotifyLiveEof()` restart; `FlushAndRestart()` clears video queue and resets audio ring buffer |
| 2026-06 | Program exits ~1s after start | Double-free in `VideoTick`: `free(frame_to_render.data)` + `VFrame::~VFrame()` frees same pointer. VFrame is a local with `~VFrame() { free(data); }`; explicit `free(frame_to_render.data)` leaves dangling pointer that destructor frees again → heap corruption | Remove explicit `free(frame_to_render.data)` — let destructor handle it |
| 2026-06 | HLS stream switch freeze (press 'n') | `HlsManager::Close()` calls `byte_stream_->Close()` which is a no-op (doesn't set `closed_=true`). Pending `BeginRead` in byte stream blocked on `data_event_` (no more `AddSegment` after download thread stops). `reader_.reset()` releases MFSourceReader → MF tries to cancel pending reads → blocked because `BeginRead` loop can't exit. Main thread freezes up to 60s. | `HlsByteStream::Abort()` sets `closed_=true` + signals `data_event_` to unblock pending reads. Called in `HlsManager::Close()` before `reader_.reset()`. |
| 2026-06 | HlsManager::Close TerminateThread risk | `TerminateThread(download_thread_)` used when download thread stuck in WinHTTP. Does not unwind stack, release locks, or free resources. If thread holds `seg_lock_`, future `EnterCriticalSection` deadlocks. | Store `active_request_` (WinHTTP handle) in HlsManager. `Close()` calls `WinHttpCloseHandle(active_request_)` to cancel pending HTTP operations — causes `WinHttpSendRequest`/`WinHttpReceiveResponse`/`WinHttpReadData` to fail immediately, letting the download thread exit cleanly. Remove `TerminateThread`. |
| 2026-06 | FlushAndRestart main-thread block 15s | `FlushAndRestart()` calls `callback_->Stop()` (5s) + `RecreateReader()` → `WaitForData(10000)` (10s). Main thread frozen for up to 15 seconds. | Reduce `Stop()` timeout to 1s, `WaitForData` timeout to 2s. Total worst-case reduced to ~3s. |
| 2026-06 | HLS network stream stops after a few minutes | (1) `cache_data_=true` for live HLS causes unbounded memory growth (consumed segments never freed). (2) Segment skip (3 failures) doesn't update `consumed_up_to_`, leaving download loop confused. (3) `FlushAndRestart` proceeds with null reader when `RecreateReader` fails, calling `StartReading()` on stale reader → immediate re-EOF → restart loop. (4) Stall detection only triggers for live/zero-duration, missing network VOD stalls. (5) For non-HLS HTTP streams, MF EOF kills the pipeline permanently with no recovery. | (1) `SetCacheData(!is_live_)` — only VOD caches data for seek-back. (2) Update `consumed_up_to_` on segment skip. (3) Abort `FlushAndRestart` if `RecreateReader` returns null → post `WM_REOPEN_SOURCE`. (4) Extend `TryRestartLivePipeline` to also trigger for `is_network_ && !is_live` → `WM_REOPEN_SOURCE`. (5) Save URL in Player; `ReopenSource()` closes everything and reopens from URL as universal stall recovery. |
