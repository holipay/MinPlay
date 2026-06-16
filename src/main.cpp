#include "util/com_ptr.h"
#include "core/player.h"
#include "util/log.h"
#include <windows.h>
#include <shellapi.h>
#include <cwchar>
#include <string>

static Player* g_player = nullptr;
static HWND    g_hwnd   = nullptr;
static int RunMessageLoop();

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (g_player) {
                g_player->Paint(hdc, rc.right, rc.bottom);
            } else {
                FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_SIZE: {
            int nw = LOWORD(lp), nh = HIWORD(lp);
            if (g_player) g_player->Resize(nw, nh);
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        }

        case WM_KEYDOWN:
            if (!g_player) break;
            switch (wp) {
                case VK_SPACE:
                    g_player->PauseToggle();
                    break;
                case VK_LEFT: {
                    double pos = g_player->GetPosition() - 10.0;
                    g_player->Seek(pos < 0 ? 0 : pos);
                    break;
                }
                case VK_RIGHT: {
                    double pos = g_player->GetPosition() + 10.0;
                    double dur = g_player->GetDuration();
                    g_player->Seek(dur > 0 && pos > dur ? dur : pos);
                    break;
                }
                case VK_ESCAPE:
                    DestroyWindow(hwnd);
                    break;
            }
            break;

        case WM_TIMER:
            if (wp == TIMER_AUDIO_CHECK && g_player)
                g_player->CheckAudio();
            if (wp == TIMER_VIDEO_DISPLAY && g_player)
                g_player->VideoTick();
            if (wp == TIMER_EOF_CHECK && g_player) {
                if (g_player->IsFinished()) {
                    KillTimer(hwnd, TIMER_EOF_CHECK);
                    DestroyWindow(hwnd);
                }
            }
            break;

        case WM_DESTROY:
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
    ApplyProcessMitigations();

    ComInit com;
    HRESULT hrMf = MFStartup(MF_VERSION, 0);

    if (!com.Succeeded() || FAILED(hrMf)) {
        MessageBoxA(nullptr, "Failed to initialize COM or Media Foundation",
                    "Error", MB_OK);
        return 1;
    }

    wchar_t url[2048] = L"test.mp4";
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv && wargc > 1) {
        wcsncpy_s(url, 2048, wargv[1], _TRUNCATE);
    }
    if (wargv) LocalFree(wargv);

    // Input validation: reject disallowed protocols
    if (!IsSchemeAllowed(url)) {
        MessageBoxA(nullptr, "Unsupported protocol. Only http://, https://, and local files are allowed.", "Security", MB_OK);
        return 1;
    }

    WNDCLASSEX wc = {sizeof(wc)};
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandle(nullptr);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = TEXT("MinPlay");
    if (!RegisterClassEx(&wc)) {
        MessageBoxA(nullptr, "Failed to register window class", "Error", MB_OK);
        return 1;
    }

    RECT rc = {0, 0, 1280, 720};
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
    if (!g_player->Open(g_hwnd, url)) {
        // Open() already logs specifics; show generic error to user
        MessageBoxA(nullptr, "Failed to open media file.\nSee console for details.", "Error", MB_OK);
        delete g_player; g_player = nullptr;
        return 1;
    }

    g_player->Play();
    if (!SetTimer(g_hwnd, TIMER_AUDIO_CHECK, 50, nullptr))
        LOG_WARN("Failed to create audio check timer");
    if (g_player->HasVideo()) {
        double fps = g_player->GetVideoFps();
        int period = fps > 0 ? (int)(1000.0 / fps) : 33;
        if (period < 1) period = 1;
        if (!SetTimer(g_hwnd, TIMER_VIDEO_DISPLAY, period, nullptr))
            LOG_WARN("Failed to create video timer");
    }
    if (!SetTimer(g_hwnd, TIMER_EOF_CHECK, 500, nullptr))
        LOG_WARN("Failed to create EOF check timer");

    /* Fire an immediate video tick to minimize A/V startup gap */
    if (g_player->HasVideo())
        PostMessage(g_hwnd, WM_TIMER, TIMER_VIDEO_DISPLAY, 0);

    LOG_INFO("Playing: %S", url);

    int ret = RunMessageLoop();

    KillTimer(g_hwnd, TIMER_AUDIO_CHECK);
    KillTimer(g_hwnd, TIMER_VIDEO_DISPLAY);
    KillTimer(g_hwnd, TIMER_EOF_CHECK);
    g_player->Close();
    delete g_player;

    MFShutdown();
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
        LOG_ERROR("SEH exception: 0x%08lX", GetExceptionCode());
        return 1;
    }
}
