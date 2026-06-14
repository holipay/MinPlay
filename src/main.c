#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <mfapi.h>
#include "core/player.h"
#include "util/log.h"

static Player*      g_player = NULL;
static HWND         g_hwnd   = NULL;

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
            return 1;

        case WM_SIZE: {
            int nw = LOWORD(lp), nh = HIWORD(lp);
            if (g_player) player_resize(g_player, nw, nh);
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        }

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

        case WM_TIMER:
            if (wp == TIMER_AUDIO_CHECK && g_player)
                player_check_audio(g_player);
            if (wp == TIMER_VIDEO_DISPLAY && g_player)
                player_video_tick(g_player);
            if (wp == TIMER_EOF_CHECK && g_player) {
                if (player_is_finished(g_player)) {
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

int main(int argc, char* argv[])
{
    FreeConsole();
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, 0);

    wchar_t url[2048] = L"test.mp4";
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv && wargc > 1)
        wcsncpy(url, wargv[1], 2039);
    if (wargv) LocalFree(wargv);

    WNDCLASSEX wc = {sizeof(wc)};
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandle(NULL);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = TEXT("MiniPlayer");
    RegisterClassEx(&wc);

    RECT rc = {0, 0, 1280, 720};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowEx(0, TEXT("MiniPlayer"), TEXT("Mini Player"),
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, GetModuleHandle(NULL), NULL);

    g_player = player_create(g_hwnd);
    if (!player_open(g_player, url)) {
        MessageBoxA(NULL, "Failed to open media file", "Error", MB_OK);
        return 1;
    }

    player_play(g_player);
    SetTimer(g_hwnd, TIMER_AUDIO_CHECK, 50, NULL);
    if (player_has_video(g_player)) {
        double fps = player_get_video_fps(g_player);
        int period = fps > 0 ? (int)(1000.0 / fps) : 33;
        if (period < 1) period = 1;
        SetTimer(g_hwnd, TIMER_VIDEO_DISPLAY, period, NULL);
    }
    SetTimer(g_hwnd, TIMER_EOF_CHECK, 500, NULL);

    /* Fire an immediate video tick to minimize A/V startup gap */
    if (player_has_video(g_player))
        PostMessage(g_hwnd, WM_TIMER, TIMER_VIDEO_DISPLAY, 0);

    LOG_INFO("Playing: %S", url);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    KillTimer(g_hwnd, TIMER_AUDIO_CHECK);
    KillTimer(g_hwnd, TIMER_VIDEO_DISPLAY);
    player_close(g_player);
    player_destroy(g_player);

    MFShutdown();
    CoUninitialize();
    return 0;
}
