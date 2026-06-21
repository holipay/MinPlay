#include "util/com_ptr.h"
#include "core/player.h"
#include "core/playlist.h"
#include "util/log.h"
#include <windows.h>
#include <shellapi.h>
#include <cwchar>
#include <string>

FILE* g_log_file = nullptr;

static Player* g_player = nullptr;
static HWND    g_hwnd   = nullptr;
static LONG WINAPI GlobalExceptionHandler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep ? ep->ExceptionRecord->ExceptionCode : 0;
    void* addr = ep ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    LOG_ERROR("Unhandled exception: 0x%08lX at %p", code, addr);
    if (g_log_file) fflush(g_log_file);
    return EXCEPTION_EXECUTE_HANDLER;
}

static int RunMessageLoop();

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (g_player) {
                g_player->DrawOSD(hdc);
            } else {
                FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_LBUTTONDOWN:
            // Delay single-click action to distinguish from double-click
            SetTimer(hwnd, TIMER_CLICK_DELAY, GetDoubleClickTime(), nullptr);
            break;

        case WM_LBUTTONDBLCLK:
            // Double-click detected: cancel pending single-click, toggle fullscreen
            KillTimer(hwnd, TIMER_CLICK_DELAY);
            if (g_player) g_player->ToggleFullscreen();
            break;

        case WM_MOUSEWHEEL:
            if (g_player) {
                int delta = GET_WHEEL_DELTA_WPARAM(wp);
                float vol = g_player->GetVolume() + (delta > 0 ? 0.05f : -0.05f);
                if (vol > 1.0f) vol = 1.0f;
                if (vol < 0.0f) vol = 0.0f;
                g_player->SetVolume(vol);
            }
            break;

        case WM_SIZE: {
            int nw = LOWORD(lp), nh = HIWORD(lp);
            if (g_player) {
                g_player->Resize(nw, nh);
                // Trigger immediate re-render so content is correct before next timer tick
                PostMessage(hwnd, WM_TIMER, TIMER_VIDEO_DISPLAY, 0);
            }
            break;
        }

        case WM_KEYDOWN:
            if (!g_player) break;
            {
                bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                switch (wp) {
                // Playback control (MPlayer: Space/p pause, q quit)
                case VK_SPACE:
                case 'p':
                case 'P':
                    g_player->PauseToggle();
                    break;
                case 'f':
                case 'F':
                    g_player->ToggleFullscreen();
                    break;
                case 'q':
                case 'Q':
                    DestroyWindow(hwnd);
                    break;

                // Seek control (MPlayer: Left/Right ±10s, Up/Down ±60s, PgUp/PgDn ±60s)
                case VK_LEFT: {
                    double step = ctrl ? 600.0 : 10.0;
                    double pos = g_player->GetPosition() - step;
                    g_player->Seek(pos < 0 ? 0 : pos);
                    break;
                }
                case VK_RIGHT: {
                    double step = ctrl ? 600.0 : 10.0;
                    double pos = g_player->GetPosition() + step;
                    double dur = g_player->GetDuration();
                    g_player->Seek(dur > 0 && pos > dur ? dur : pos);
                    break;
                }
                case VK_UP: {
                    double step = ctrl ? 600.0 : 60.0;
                    double pos = g_player->GetPosition() + step;
                    double dur = g_player->GetDuration();
                    g_player->Seek(dur > 0 && pos > dur ? dur : pos);
                    break;
                }
                case VK_DOWN: {
                    double step = ctrl ? 600.0 : 60.0;
                    double pos = g_player->GetPosition() - step;
                    g_player->Seek(pos < 0 ? 0 : pos);
                    break;
                }
                case VK_PRIOR: { // Page Up
                    double pos = g_player->GetPosition() - 60.0;
                    g_player->Seek(pos < 0 ? 0 : pos);
                    break;
                }
                case VK_NEXT: { // Page Down
                    double pos = g_player->GetPosition() + 60.0;
                    double dur = g_player->GetDuration();
                    g_player->Seek(dur > 0 && pos > dur ? dur : pos);
                    break;
                }
                case VK_HOME:
                    g_player->Seek(0);
                    break;
                case VK_END: {
                    double dur = g_player->GetDuration();
                    g_player->Seek(dur > 0 ? dur : 0);
                    break;
                }

                // Volume control (MPlayer: 0 up, 9 down, / up, * down)
                case '0': {
                    float vol = g_player->GetVolume() + 0.10f;
                    if (vol > 1.0f) vol = 1.0f;
                    g_player->SetVolume(vol);
                    break;
                }
                case '9': {
                    float vol = g_player->GetVolume() - 0.10f;
                    if (vol < 0.0f) vol = 0.0f;
                    g_player->SetVolume(vol);
                    break;
                }
                case VK_OEM_2: { // / key — volume up
                    float vol = g_player->GetVolume() + 0.10f;
                    if (vol > 1.0f) vol = 1.0f;
                    g_player->SetVolume(vol);
                    break;
                }
                case VK_MULTIPLY: { // * key — volume down
                    float vol = g_player->GetVolume() - 0.10f;
                    if (vol < 0.0f) vol = 0.0f;
                    g_player->SetVolume(vol);
                    break;
                }
                case '+':
                case '=': {
                    float vol = g_player->GetVolume() + 0.01f;
                    if (vol > 1.0f) vol = 1.0f;
                    g_player->SetVolume(vol);
                    break;
                }
                case '-': {
                    float vol = g_player->GetVolume() - 0.01f;
                    if (vol < 0.0f) vol = 0.0f;
                    g_player->SetVolume(vol);
                    break;
                }
                case 'm':
                case 'M':
                    g_player->ToggleMute();
                    break;

                // Fullscreen & exit
                case VK_F11:
                    g_player->ToggleFullscreen();
                    break;
                case VK_ESCAPE:
                    if (g_player->IsFullscreen())
                        g_player->ToggleFullscreen();
                    else
                        DestroyWindow(hwnd);
                    break;

                // Playlist: next/prev track (MPlayer: > next, < prev)
                case 'n':
                case 'N':
                    g_player->PlayNext();
                    break;
                case VK_OEM_PERIOD: { // > key (shift+.)
                    if (GetKeyState(VK_SHIFT) & 0x8000) {
                        g_player->PlayNext();
                    } else {
                        // Frame stepping
                        if (g_player->GetState() != PlayerState::Paused)
                            g_player->PauseToggle();
                        double fps = g_player->GetVideoFps();
                        if (fps > 0) {
                            double pos = g_player->GetPosition() + 1.0 / fps;
                            double dur = g_player->GetDuration();
                            g_player->Seek(dur > 0 && pos > dur ? dur : pos);
                        }
                    }
                    break;
                }
                case VK_OEM_COMMA: { // < key (shift+,)
                    if (GetKeyState(VK_SHIFT) & 0x8000) {
                        g_player->PlayPrev();
                    } else {
                        // Frame stepping
                        if (g_player->GetState() != PlayerState::Paused)
                            g_player->PauseToggle();
                        double fps = g_player->GetVideoFps();
                        if (fps > 0) {
                            double pos = g_player->GetPosition() - 1.0 / fps;
                            g_player->Seek(pos < 0 ? 0 : pos);
                        }
                    }
                    break;
                }
                }
            }
            break;

        case WM_TIMER:
            if (wp == TIMER_AUDIO_CHECK && g_player)
                g_player->CheckAudio();
            if (wp == TIMER_VIDEO_DISPLAY && g_player)
                g_player->VideoTick();
            if (wp == TIMER_EOF_CHECK && g_player) {
                if (g_player->IsFinished()) {
                    LOG_INFO("IsFinished() returned true, closing");
                    if (g_player->HasPlaylist()) {
                        g_player->OnTrackFinished();
                        // Re-arm EOF timer for next track
                        SetTimer(hwnd, TIMER_EOF_CHECK, 500, nullptr);
                    } else {
                        KillTimer(hwnd, TIMER_EOF_CHECK);
                        DestroyWindow(hwnd);
                    }
                }
            }
            if (wp == TIMER_CLICK_DELAY) {
                // Single-click confirmed (no double-click within timeout)
                KillTimer(hwnd, TIMER_CLICK_DELAY);
                if (g_player) g_player->PauseToggle();
            }
            if (wp == TIMER_OSD_REFRESH && g_player) {
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;

        case WM_OPEN_COMPLETE:
            if (!g_player) break;
            // Discard stale WM_OPEN_COMPLETE from a previous Open() call
            if ((int)lp != g_player->GetOpenGeneration()) break;
            if (wp == 0 || !g_player->IsOpenSuccessful()) {
                if (g_player->HasPlaylist()) {
                    // Skip to next track on failure
                    LOG_WARN("Track failed to open, trying next...");
                    g_player->OnTrackFinished();
                    SetTimer(hwnd, TIMER_EOF_CHECK, 500, nullptr);
                } else {
                    KillTimer(hwnd, TIMER_EOF_CHECK);
                    DestroyWindow(hwnd);
                }
                break;
            }
            SetWindowText(hwnd, TEXT("MinPlay"));
            g_player->Play();
            // Audio ring-buffer refill timer (Play() handles video timer internally)
            if (!SetTimer(hwnd, TIMER_AUDIO_CHECK, 50, nullptr))
                LOG_WARN("Failed to create audio check timer");
            // OSD refresh timer for audio-only mode (no D3D11 to drive repaints)
            if (g_player->IsAudioOnly()) {
                if (!SetTimer(hwnd, TIMER_OSD_REFRESH, 500, nullptr))
                    LOG_WARN("Failed to create OSD refresh timer");
            }
            break;

        case WM_RESTART_LIVE:
            if (g_player)
                g_player->FlushAndRestart();
            break;

        case WM_REOPEN_SOURCE:
            if (g_player)
                g_player->ReopenSource();
            break;

        case WM_PLAYLIST_DONE:
            LOG_INFO("WM_PLAYLIST_DONE received");
            KillTimer(hwnd, TIMER_EOF_CHECK);
            DestroyWindow(hwnd);
            break;

        case WM_DESTROY:
            LOG_INFO("WM_DESTROY received");
            PostQuitMessage(0);
            break;

        default:
            return DefWindowProc(hwnd, msg, wp, lp);
    }
    return 0;
}

/* ----- Security: input validation ----- */

// Allowed URI schemes for media playback
static bool IsSchemeAllowed(const wchar_t* url) {
    if (!url || !*url) return false;
    size_t len = wcsnlen_s(url, 2048);
    if (len == 0 || len >= 2048) return false;

    std::wstring s(url);
    size_t prot = s.find(L"://");
    if (prot == std::wstring::npos) return true; // local file — allowed

    std::wstring scheme = s.substr(0, prot);
    return _wcsicmp(scheme.c_str(), L"http")  == 0 ||
           _wcsicmp(scheme.c_str(), L"https") == 0;
}

/* ----- Security: process hardening ----- */
static void ApplyProcessMitigations() {
    // Fail-fast on heap corruption
    HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);

    // Block AppInit DLL injection
    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY ep = {};
    ep.DisableExtensionPoints = TRUE;
    if (!SetProcessMitigationPolicy(ProcessExtensionPointDisablePolicy, &ep, sizeof(ep)))
        LOG_WARN("ExtensionPointDisablePolicy not applied (0x%08lX)", GetLastError());

    // Raise exception on CloseHandle(INVALID_HANDLE_VALUE) — catches handle misuse
    PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY sh = {};
    sh.RaiseExceptionOnInvalidHandleReference = TRUE;
    sh.HandleExceptionsPermanentlyEnabled = TRUE;
    if (!SetProcessMitigationPolicy(ProcessStrictHandleCheckPolicy, &sh, sizeof(sh)))
        LOG_WARN("StrictHandleCheckPolicy not applied (0x%08lX)", GetLastError());
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
#if CURRENT_LOG_LEVEL <= LOG_LEVEL_DEBUG
    // Allocate console for log output in debug builds only
    AllocConsole();
    FILE* dummy;
    freopen_s(&dummy, "CONOUT$", "w", stderr);
    freopen_s(&dummy, "CONOUT$", "w", stdout);
#endif

    fopen_s(&g_log_file, "minplay.log", "w");
    if (g_log_file) LOG_INFO("MinPlay starting (log file: minplay.log)");

    ApplyProcessMitigations();

    SetUnhandledExceptionFilter(GlobalExceptionHandler);

    ComInit com;
    HRESULT hrMf = MFStartup(MF_VERSION, 0);

    if (!com.Succeeded() || FAILED(hrMf)) {
        MessageBoxA(nullptr, "Failed to initialize COM or Media Foundation",
                    "Error", MB_OK);
        return 1;
    }

    wchar_t url[2048] = L"test.mp4";
    bool audio_only = false;
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv) {
        for (int i = 1; i < wargc; i++) {
            if (_wcsicmp(wargv[i], L"--audio-only") == 0 || _wcsicmp(wargv[i], L"-a") == 0) {
                audio_only = true;
            } else {
                wcsncpy_s(url, 2048, wargv[i], _TRUNCATE);
            }
        }
    }
    if (wargv) LocalFree(wargv);

    // Input validation: reject disallowed protocols
    if (!IsSchemeAllowed(url)) {
        MessageBoxA(nullptr, "Unsupported protocol. Only http://, https://, and local files are allowed.", "Security", MB_OK);
        return 1;
    }

    // Detect playlist file (.m3u/.m3u8 extension, local only)
    bool is_playlist = false;
    PlaylistManager playlist;
    {
        std::wstring wurl(url);
        if (wurl.find(L"://") == std::wstring::npos) {
            size_t dot = wurl.rfind(L'.');
            if (dot != std::wstring::npos) {
                std::wstring ext = wurl.substr(dot);
                if (_wcsicmp(ext.c_str(), L".m3u") == 0 ||
                    _wcsicmp(ext.c_str(), L".m3u8") == 0)
                    is_playlist = true;
            }
        }
        if (is_playlist) {
            if (!playlist.LoadFromFile(url)) {
                MessageBoxA(nullptr, "Failed to parse playlist file.", "Error", MB_OK);
                return 1;
            }
            wcsncpy_s(url, 2048, playlist.GetCurrent().url.c_str(), _TRUNCATE);
            if (!IsSchemeAllowed(url)) {
                MessageBoxA(nullptr, "Unsupported protocol in playlist entry.", "Security", MB_OK);
                return 1;
            }
        }
    }

    WNDCLASSEX wc = {sizeof(wc)};
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandle(nullptr);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = TEXT("MinPlay");
    if (!RegisterClassEx(&wc)) {
        MessageBoxA(nullptr, "Failed to register window class", "Error", MB_OK);
        return 1;
    }

    RECT rc = {0, 0, audio_only ? 400 : 1280, audio_only ? 120 : 720};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowEx(0, TEXT("MinPlay"), TEXT("MinPlay"),
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (!g_hwnd) {
        MessageBoxA(nullptr, "Failed to create window", "Error", MB_OK);
        return 1;
    }

    g_player = new (std::nothrow) Player();
    if (!g_player) {
        MessageBoxA(nullptr, "Out of memory", "Error", MB_OK);
        return 1;
    }
    if (is_playlist)
        g_player->SetPlaylist(&playlist);
    SetWindowText(g_hwnd, TEXT("MinPlay - Loading..."));
    bool open_ok = g_player->Open(g_hwnd, url, audio_only);
    if (!open_ok) {
        MessageBoxA(nullptr, "Failed to open media file.\nSee console for details.", "Error", MB_OK);
        delete g_player; g_player = nullptr;
        return 1;
    }

    // EOF check timer watches for open timeout and playback completion
    if (!SetTimer(g_hwnd, TIMER_EOF_CHECK, 500, nullptr))
        LOG_WARN("Failed to create EOF check timer");

    LOG_INFO("Opening: %S", url);

    int ret = RunMessageLoop();

    LOG_INFO("Message loop ended, cleaning up (ret=%d)", ret);
    KillTimer(g_hwnd, TIMER_AUDIO_CHECK);
    KillTimer(g_hwnd, TIMER_VIDEO_DISPLAY);
    KillTimer(g_hwnd, TIMER_EOF_CHECK);
    KillTimer(g_hwnd, TIMER_CLICK_DELAY);
    KillTimer(g_hwnd, TIMER_OSD_REFRESH);
    delete g_player;
    g_player = nullptr;

    MFShutdown();
    LOG_INFO("MinPlay exiting");
    if (g_log_file) { fclose(g_log_file); g_log_file = nullptr; }
    return ret;
}

static int RunMessageLoop() {
    __try {
    /*
     * Standard Windows message loop.
     * GetMessage() blocks when the queue is empty — thread sleeps at 0% CPU.
     * WM_TIMER messages (50ms audio, ~33ms video) wake it periodically.
     * No Sleep() needed: the kernel wait IS the sleep.
     * Sleep() in the loop would reduce responsiveness without power benefit.
     */
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        DWORD code = GetExceptionCode();
        LOG_ERROR("SEH exception: 0x%08lX", code);
        if (g_log_file) fflush(g_log_file);
        return 1;
    }
}
