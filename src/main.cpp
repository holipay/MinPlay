#include "util/com_ptr.h"
#include "core/player.h"
#include "util/log.h"
#include <windows.h>
#include <shellapi.h>
#include <cwchar>

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

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
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

    WNDCLASSEX wc = {sizeof(wc)};
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandle(nullptr);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = TEXT("MinPlay");
    RegisterClassEx(&wc);

    RECT rc = {0, 0, 1280, 720};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowEx(0, TEXT("MinPlay"), TEXT("MinPlay"),
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, GetModuleHandle(nullptr), nullptr);

    g_player = new (std::nothrow) Player();
    if (!g_player) {
        MessageBoxA(nullptr, "Out of memory", "Error", MB_OK);
        return 1;
    }
    if (!g_player->Open(g_hwnd, url)) {
        MessageBoxA(nullptr, "Failed to open media file", "Error", MB_OK);
        delete g_player; g_player = nullptr;
        return 1;
    }

    g_player->Play();
    SetTimer(g_hwnd, TIMER_AUDIO_CHECK, 50, nullptr);
    if (g_player->HasVideo()) {
        double fps = g_player->GetVideoFps();
        int period = fps > 0 ? (int)(1000.0 / fps) : 33;
        if (period < 1) period = 1;
        SetTimer(g_hwnd, TIMER_VIDEO_DISPLAY, period, nullptr);
    }
    SetTimer(g_hwnd, TIMER_EOF_CHECK, 500, nullptr);

    /* Fire an immediate video tick to minimize A/V startup gap */
    if (g_player->HasVideo())
        PostMessage(g_hwnd, WM_TIMER, TIMER_VIDEO_DISPLAY, 0);

    LOG_INFO("Playing: %S", url);

    int ret = RunMessageLoop();

    KillTimer(g_hwnd, TIMER_AUDIO_CHECK);
    KillTimer(g_hwnd, TIMER_VIDEO_DISPLAY);
    g_player->Close();
    delete g_player;

    MFShutdown();
    return ret;
}

static int RunMessageLoop() {
    __try {
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
