#include "overlay.h"
#include "memory.h"
#include <iostream>
#include <string>

// Global overlay state
HWND g_hOverlayWnd = nullptr;
HANDLE g_hOverlayThread = nullptr;
bool g_OverlayRunning = false;
HWND g_hGameWnd = nullptr;

// Forward declaration of WndProc
LRESULT CALLBACK OverlayWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

struct EnumData {
    DWORD dwProcessId;
    HWND hWnd;
};

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    EnumData& data = *(EnumData*)lParam;
    DWORD dwProcessId = 0;
    GetWindowThreadProcessId(hwnd, &dwProcessId);
    if (dwProcessId == data.dwProcessId && GetParent(hwnd) == NULL && IsWindowVisible(hwnd)) {
        data.hWnd = hwnd;
        return FALSE; // Found
    }
    return TRUE;
}

HWND GetGameWindow() {
    EnumData data = { GetCurrentProcessId(), NULL };
    EnumWindows(EnumWindowsProc, (LPARAM)&data);
    return data.hWnd;
}

// Draw a progress bar using GDI
void DrawProgressBar(HDC hdc, int x, int y, int width, int height, float percentage, COLORREF color) {
    // Draw background (dark gray)
    HBRUSH bgBrush = CreateSolidBrush(RGB(50, 50, 50));
    RECT bgRect = { x, y, x + width, y + height };
    FillRect(hdc, &bgRect, bgBrush);
    DeleteObject(bgBrush);

    // Draw fill
    if (percentage > 1.0f) percentage = 1.0f;
    if (percentage < 0.0f) percentage = 0.0f;
    
    if (percentage > 0.0f) {
        HBRUSH fillBrush = CreateSolidBrush(color);
        RECT fillRect = { x, y, x + (int)(width * percentage), y + height };
        FillRect(hdc, &fillRect, fillBrush);
        DeleteObject(fillBrush);
    }

    // Draw border
    HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(100, 100, 100));
    HPEN oldPen = (HPEN)SelectObject(hdc, borderPen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    
    Rectangle(hdc, x, y, x + width, y + height);
    
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(borderPen);
}

LRESULT CALLBACK OverlayWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);

            // Double buffering to prevent flickering
            RECT clientRect;
            GetClientRect(hWnd, &clientRect);
            int screenWidth = clientRect.right - clientRect.left;
            int screenHeight = clientRect.bottom - clientRect.top;

            HDC hdcMem = CreateCompatibleDC(hdc);
            HBITMAP hbmMem = CreateCompatibleBitmap(hdc, screenWidth, screenHeight);
            HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbmMem);

            // Fill background with transparent color key (black)
            HBRUSH bgBrush = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(hdcMem, &clientRect, bgBrush);
            DeleteObject(bgBrush);

            if (IsInBattle()) {
                // Draw a nice semi-transparent black panel on the top left
                HBRUSH panelBrush = CreateSolidBrush(RGB(15, 15, 20));
                RECT panelRect = { 10, 10, 520, 420 };
                FillRect(hdcMem, &panelRect, panelBrush);
                DeleteObject(panelBrush);

                // Panel border
                HPEN panelPen = CreatePen(PS_SOLID, 2, RGB(0, 122, 204));
                HPEN oldPen = (HPEN)SelectObject(hdcMem, panelPen);
                HBRUSH oldBrush = (HBRUSH)SelectObject(hdcMem, GetStockObject(NULL_BRUSH));
                Rectangle(hdcMem, 10, 10, 520, 420);
                SelectObject(hdcMem, oldPen);
                SelectObject(hdcMem, oldBrush);
                DeleteObject(panelPen);

                // Set up fonts and text drawing parameters
                HFONT hFontTitle = CreateFontA(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Consolas");
                HFONT hFontText = CreateFontA(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Consolas");
                
                SetBkMode(hdcMem, TRANSPARENT);

                // Draw Title
                SelectObject(hdcMem, hFontTitle);
                SetTextColor(hdcMem, RGB(0, 192, 255));
                TextOutA(hdcMem, 25, 20, "FFVII BATTLE ANALYZER", 21);

                SelectObject(hdcMem, hFontText);

                int yOffset = 55;

                // 1. Draw Playable Characters (Indices 0 - 2)
                SetTextColor(hdcMem, RGB(255, 255, 255));
                TextOutA(hdcMem, 25, yOffset, "--- ALLIES ---", 14);
                yOffset += 20;

                for (int i = 0; i < 3; i++) {
                    ActorData data;
                    if (GetActorData(i, data) && data.is_active) {
                        char buf[128];
                        snprintf(buf, sizeof(buf), "Ally %d: HP %d/%d | MP %d/%d", i + 1, data.current_hp, data.max_hp, data.current_mp, data.max_mp);
                        SetTextColor(hdcMem, RGB(230, 230, 230));
                        TextOutA(hdcMem, 25, yOffset, buf, (int)strlen(buf));
                        
                        // HP progress bar
                        float hpPercent = data.max_hp > 0 ? (float)data.current_hp / data.max_hp : 0.0f;
                        DrawProgressBar(hdcMem, 25, yOffset + 18, 200, 8, hpPercent, RGB(0, 200, 80));

                        // ATB progress bar (max ATB value is usually 65535)
                        float atbPercent = (float)data.atb / 65535.0f;
                        DrawProgressBar(hdcMem, 240, yOffset + 18, 200, 8, atbPercent, RGB(255, 140, 0));

                        yOffset += 32;
                    }
                }

                yOffset += 10;

                // 2. Draw Enemies (Indices 4 - 9)
                SetTextColor(hdcMem, RGB(255, 255, 255));
                TextOutA(hdcMem, 25, yOffset, "--- ENEMIES ---", 15);
                yOffset += 20;

                int enemyCount = 0;
                for (int i = 4; i < 10; i++) {
                    ActorData data;
                    if (GetActorData(i, data) && data.is_active) {
                        enemyCount++;
                        char buf[128];
                        snprintf(buf, sizeof(buf), "Enemy %d: HP %d/%d | MP %d/%d", enemyCount, data.current_hp, data.max_hp, data.current_mp, data.max_mp);
                        SetTextColor(hdcMem, RGB(255, 80, 80));
                        TextOutA(hdcMem, 25, yOffset, buf, (int)strlen(buf));

                        // HP progress bar
                        float hpPercent = data.max_hp > 0 ? (float)data.current_hp / data.max_hp : 0.0f;
                        DrawProgressBar(hdcMem, 25, yOffset + 18, 180, 8, hpPercent, RGB(220, 40, 40));

                        // ATB progress bar
                        float atbPercent = (float)data.atb / 65535.0f;
                        DrawProgressBar(hdcMem, 220, yOffset + 18, 100, 8, atbPercent, RGB(255, 140, 0));

                        // Steal Status
                        if (data.stolen) {
                            SetTextColor(hdcMem, RGB(120, 120, 120));
                            TextOutA(hdcMem, 335, yOffset + 14, "[Stolen]", 8);
                        } else {
                            SetTextColor(hdcMem, RGB(0, 255, 120));
                            TextOutA(hdcMem, 335, yOffset + 14, "[Can Steal]", 11);
                        }

                        // Sensed indicator
                        if (data.sensed) {
                            SetTextColor(hdcMem, RGB(180, 180, 0));
                            TextOutA(hdcMem, 435, yOffset + 14, "[Sensed]", 8);
                        }

                        yOffset += 32;
                    }
                }

                if (enemyCount == 0) {
                    SetTextColor(hdcMem, RGB(150, 150, 150));
                    TextOutA(hdcMem, 25, yOffset, "(Scanning for active enemies...)", 32);
                }

                DeleteObject(hFontTitle);
                DeleteObject(hFontText);
            }

            // Copy memory DC to screen DC
            BitBlt(hdc, 0, 0, screenWidth, screenHeight, hdcMem, 0, 0, SRCCOPY);

            SelectObject(hdcMem, hbmOld);
            DeleteObject(hbmMem);
            DeleteDC(hdcMem);

            EndPaint(hWnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1; // Prevent background erasing to maintain transparency
    }
    return DefWindowProc(hWnd, message, wParam, lParam);
}

void DebugLog(const char* format, ...);

DWORD WINAPI OverlayThread(LPVOID lpParam) {
    DebugLog("[BattleOverlay] OverlayThread started.\n");

    // Wait for the game window to load and show up
    while (g_OverlayRunning) {
        g_hGameWnd = GetGameWindow();
        if (g_hGameWnd != NULL) {
            DebugLog("[BattleOverlay] Game window found: %p\n", g_hGameWnd);
            break;
        }
        Sleep(500);
    }

    if (!g_OverlayRunning) {
        DebugLog("[BattleOverlay] OverlayThread stopped before window creation.\n");
        return 0;
    }

    // Register class
    WNDCLASSEXA wc = { 0 };
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "FFVIISE_BattleOverlay";
    RegisterClassExA(&wc);

    // Get game window rect
    RECT rect;
    GetWindowRect(g_hGameWnd, &rect);
    DebugLog("[BattleOverlay] Game window rect: left=%d, top=%d, right=%d, bottom=%d\n", rect.left, rect.top, rect.right, rect.bottom);

    // Create transparent borderless overlay window
    g_hOverlayWnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED,
        "FFVIISE_BattleOverlay", "", WS_POPUP,
        rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
        g_hGameWnd, NULL, wc.hInstance, NULL
    );

    if (!g_hOverlayWnd) {
        DebugLog("[BattleOverlay] ERROR: Failed to create overlay window!\n");
        return 0;
    }

    DebugLog("[BattleOverlay] Overlay window created successfully: %p\n", g_hOverlayWnd);

    // Use black as the transparent color key, and set content opacity using INI config setting (g_Opacity)
    SetLayeredWindowAttributes(g_hOverlayWnd, RGB(0, 0, 0), g_Opacity, LWA_COLORKEY | LWA_ALPHA);
    ShowWindow(g_hOverlayWnd, SW_SHOW);
    UpdateWindow(g_hOverlayWnd);

    MSG msg;
    while (g_OverlayRunning) {
        // Track the game window size and position
        if (IsWindow(g_hGameWnd)) {
            RECT gameRect;
            GetWindowRect(g_hGameWnd, &gameRect);
            
            // Adjust overlay window position to match game window
            SetWindowPos(g_hOverlayWnd, HWND_TOPMOST, 
                         gameRect.left, gameRect.top, 
                         gameRect.right - gameRect.left, 
                         gameRect.bottom - gameRect.top, 
                         SWP_NOACTIVATE);
        } else {
            // Game window closed, exit thread
            break;
        }

        // Process message queue
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        // Redraw
        InvalidateRect(g_hOverlayWnd, NULL, FALSE);
        Sleep(33); // 30 FPS update rate
    }

    DestroyWindow(g_hOverlayWnd);
    UnregisterClassA("FFVIISE_BattleOverlay", wc.hInstance);
    return 0;
}

void StartOverlayThread() {
    g_OverlayRunning = true;
    g_hOverlayThread = CreateThread(NULL, 0, OverlayThread, NULL, 0, NULL);
}

void StopOverlayThread() {
    g_OverlayRunning = false;
    if (g_hOverlayThread) {
        WaitForSingleObject(g_hOverlayThread, 2000);
        CloseHandle(g_hOverlayThread);
        g_hOverlayThread = nullptr;
    }
}
