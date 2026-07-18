#include "overlay.h"
#include "memory.h"
#include <iostream>
#include <string>

// Global overlay state
HWND g_hOverlayWnd = nullptr;
HANDLE g_hOverlayThread = nullptr;
bool g_OverlayRunning = false;
HWND g_hGameWnd = nullptr;
bool g_ShowOverlay = true;

// Diagnostics functions from Loader (d3d11.dll)
typedef int(*GetActiveModsCount_t)();
typedef void(*GetActiveModName_t)(int, wchar_t*, int);
typedef int(*GetActivePluginsCount_t)();
typedef void(*GetActivePluginInfo_t)(int, wchar_t*, void**);
typedef void(*GetLoaderDiagnosticInfo_t)(int*, int*, int*);

GetActiveModsCount_t fnGetActiveModsCount = nullptr;
GetActiveModName_t fnGetActiveModName = nullptr;
GetActivePluginsCount_t fnGetActivePluginsCount = nullptr;
GetActivePluginInfo_t fnGetActivePluginInfo = nullptr;
GetLoaderDiagnosticInfo_t fnGetLoaderDiagnosticInfo = nullptr;

bool g_DevPanelVisible = false;


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
                // Draw a nice semi-transparent black panel
                HBRUSH panelBrush = CreateSolidBrush(g_PanelBgColor);
                RECT panelRect = { g_PanelX, g_PanelY, g_PanelX + g_PanelWidth, g_PanelY + g_PanelHeight };
                FillRect(hdcMem, &panelRect, panelBrush);
                DeleteObject(panelBrush);

                // Panel border
                HPEN panelPen = CreatePen(PS_SOLID, 2, g_PanelBorderColor);
                HPEN oldPen = (HPEN)SelectObject(hdcMem, panelPen);
                HBRUSH oldBrush = (HBRUSH)SelectObject(hdcMem, GetStockObject(NULL_BRUSH));
                Rectangle(hdcMem, g_PanelX, g_PanelY, g_PanelX + g_PanelWidth, g_PanelY + g_PanelHeight);
                SelectObject(hdcMem, oldPen);
                SelectObject(hdcMem, oldBrush);
                DeleteObject(panelPen);

                // Set up fonts and text drawing parameters
                HFONT hFontTitle = CreateFontA(g_TitleFontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, g_FontFace);
                HFONT hFontText = CreateFontA(g_TextFontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, g_FontFace);
                
                SetBkMode(hdcMem, TRANSPARENT);

                // Draw Title
                SelectObject(hdcMem, hFontTitle);
                SetTextColor(hdcMem, g_TitleColor);
                TextOutA(hdcMem, g_PanelX + 15, g_PanelY + 10, "FFVII BATTLE ANALYZER", 21);

                SelectObject(hdcMem, hFontText);

                int yOffset = g_PanelY + g_TitleFontSize + 25;

                // 1. Draw Playable Characters (Indices 0 - 2)
                if (g_ShowParty) {
                    SetTextColor(hdcMem, g_TextColorHeader);
                    TextOutA(hdcMem, g_PanelX + 15, yOffset, "--- ALLIES ---", 14);
                    yOffset += g_TextFontSize + 4;

                    for (int i = 0; i < 3; i++) {
                        ActorData data;
                        if (GetActorData(i, data) && data.is_active) {
                            char buf[128];
                            snprintf(buf, sizeof(buf), "Ally %d: HP %d/%d | MP %d/%d", i + 1, data.current_hp, data.max_hp, data.current_mp, data.max_mp);
                            SetTextColor(hdcMem, g_TextColorAlly);
                            TextOutA(hdcMem, g_PanelX + 15, yOffset, buf, (int)strlen(buf));
                            
                            // HP progress bar
                            float hpPercent = data.max_hp > 0 ? (float)data.current_hp / data.max_hp : 0.0f;
                            DrawProgressBar(hdcMem, g_PanelX + 15, yOffset + g_TextFontSize + 2, 200, 8, hpPercent, g_PlayerHPColor);

                            // ATB progress bar (max ATB value is usually 65535)
                            if (g_ShowATB) {
                                float atbPercent = (float)data.atb / 65535.0f;
                                DrawProgressBar(hdcMem, g_PanelX + 230, yOffset + g_TextFontSize + 2, 200, 8, atbPercent, g_ATBColor);
                            }

                            // Numeric HP overlay if enabled
                            if (g_ShowNumericHP) {
                                char hpBuf[32];
                                snprintf(hpBuf, sizeof(hpBuf), "%d/%d", data.current_hp, data.max_hp);
                                SetTextColor(hdcMem, g_TextColorAlly);
                                TextOutA(hdcMem, g_PanelX + 15 + 205, yOffset + g_TextFontSize - 2, hpBuf, (int)strlen(hpBuf));
                            }

                            yOffset += g_TextFontSize + 16;
                        }
                    }
                    yOffset += 10;
                }

                // 2. Draw Enemies (Indices 4 - 9)
                if (g_ShowEnemies) {
                    SetTextColor(hdcMem, g_TextColorHeader);
                    TextOutA(hdcMem, g_PanelX + 15, yOffset, "--- ENEMIES ---", 15);
                    yOffset += g_TextFontSize + 4;

                    int enemyCount = 0;
                    for (int i = 4; i < 10; i++) {
                        ActorData data;
                        if (GetActorData(i, data) && data.is_active) {
                            enemyCount++;
                            char buf[128];
                            snprintf(buf, sizeof(buf), "Enemy %d: HP %d/%d | MP %d/%d", enemyCount, data.current_hp, data.max_hp, data.current_mp, data.max_mp);
                            SetTextColor(hdcMem, g_TextColorEnemy);
                            TextOutA(hdcMem, g_PanelX + 15, yOffset, buf, (int)strlen(buf));

                            // HP progress bar
                            float hpPercent = data.max_hp > 0 ? (float)data.current_hp / data.max_hp : 0.0f;
                            DrawProgressBar(hdcMem, g_PanelX + 15, yOffset + g_TextFontSize + 2, 180, 8, hpPercent, g_EnemyHPColor);

                            // ATB progress bar
                            if (g_ShowATB) {
                                float atbPercent = (float)data.atb / 65535.0f;
                                DrawProgressBar(hdcMem, g_PanelX + 210, yOffset + g_TextFontSize + 2, 100, 8, atbPercent, g_ATBColor);
                            }

                            // Numeric HP overlay if enabled
                            if (g_ShowNumericHP) {
                                char hpBuf[32];
                                snprintf(hpBuf, sizeof(hpBuf), "%d/%d", data.current_hp, data.max_hp);
                                SetTextColor(hdcMem, g_TextColorEnemy);
                                TextOutA(hdcMem, g_PanelX + 15 + 185, yOffset + g_TextFontSize - 2, hpBuf, (int)strlen(hpBuf));
                            }

                            // Steal Status
                            if (data.stolen) {
                                SetTextColor(hdcMem, RGB(120, 120, 120));
                                TextOutA(hdcMem, g_PanelX + 325, yOffset + g_TextFontSize - 2, "[Stolen]", 8);
                            } else {
                                SetTextColor(hdcMem, RGB(0, 255, 120));
                                TextOutA(hdcMem, g_PanelX + 325, yOffset + g_TextFontSize - 2, "[Can Steal]", 11);
                            }

                            // Sensed indicator
                            if (data.sensed) {
                                SetTextColor(hdcMem, RGB(180, 180, 0));
                                TextOutA(hdcMem, g_PanelX + 435, yOffset + g_TextFontSize - 2, "[Sensed]", 8);
                            }

                            yOffset += g_TextFontSize + 16;
                        }
                    }

                    if (enemyCount == 0) {
                        SetTextColor(hdcMem, RGB(150, 150, 150));
                        TextOutA(hdcMem, g_PanelX + 15, yOffset, "(Scanning for active enemies...)", 32);
                    }
                }

                // Draw FPS if enabled
                if (g_ShowFPS) {
                    static DWORD lastTime = GetTickCount();
                    static int frameCount = 0;
                    static int fps = 60;
                    
                    frameCount++;
                    DWORD currentTime = GetTickCount();
                    if (currentTime - lastTime >= 1000) {
                        fps = frameCount;
                        frameCount = 0;
                        lastTime = currentTime;
                    }
                    
                    char fpsBuf[32];
                    snprintf(fpsBuf, sizeof(fpsBuf), "FPS: %d", fps);
                    SetTextColor(hdcMem, RGB(0, 255, 120));
                    int fpsX = g_PanelX + g_PanelWidth - 85;
                    int fpsY = g_PanelY + 10;
                    TextOutA(hdcMem, fpsX, fpsY, fpsBuf, (int)strlen(fpsBuf));
                }

                // Draw Developer Diagnostics Panel if enabled and toggled visible
                if (g_ShowDeveloperPanel && g_DevPanelVisible) {
                    int devX = g_PanelX + g_PanelWidth + 10;
                    int devY = g_PanelY;
                    int devWidth = 400;
                    int devHeight = g_PanelHeight;

                    // Panel background (dark charcoal)
                    HBRUSH devBgBrush = CreateSolidBrush(RGB(20, 20, 25));
                    RECT devRect = { devX, devY, devX + devWidth, devY + devHeight };
                    FillRect(hdcMem, &devRect, devBgBrush);
                    DeleteObject(devBgBrush);

                    // Panel border (warm developer orange)
                    HPEN devPen = CreatePen(PS_SOLID, 2, RGB(220, 100, 50));
                    HPEN oldPen = (HPEN)SelectObject(hdcMem, devPen);
                    HBRUSH oldBrush = (HBRUSH)SelectObject(hdcMem, GetStockObject(NULL_BRUSH));
                    Rectangle(hdcMem, devX, devY, devX + devWidth, devY + devHeight);
                    SelectObject(hdcMem, oldPen);
                    SelectObject(hdcMem, oldBrush);
                    DeleteObject(devPen);

                    // Title
                    SelectObject(hdcMem, hFontTitle);
                    SetTextColor(hdcMem, RGB(255, 120, 50));
                    TextOutA(hdcMem, devX + 15, devY + 10, "DEVELOPER DIAGNOSTICS", 21);

                    SelectObject(hdcMem, hFontText);
                    int devYOffset = devY + g_TitleFontSize + 25;

                    // Read Loader diagnostics if available
                    int d3dHooked = 0, loggingEnabled = 0, verboseLogging = 0;
                    if (fnGetLoaderDiagnosticInfo) {
                        fnGetLoaderDiagnosticInfo(&d3dHooked, &loggingEnabled, &verboseLogging);
                    }

                    char stateBuf[128];
                    snprintf(stateBuf, sizeof(stateBuf), "Loader Status: %s", fnGetLoaderDiagnosticInfo ? "ACTIVE" : "NO API EXPORTS");
                    SetTextColor(hdcMem, RGB(220, 220, 220));
                    TextOutA(hdcMem, devX + 15, devYOffset, stateBuf, (int)strlen(stateBuf));
                    devYOffset += g_TextFontSize + 6;

                    SetTextColor(hdcMem, RGB(180, 180, 180));
                    snprintf(stateBuf, sizeof(stateBuf), "- D3D11 Hooks: %s", d3dHooked ? "HOOKED" : "DISABLED");
                    TextOutA(hdcMem, devX + 15, devYOffset, stateBuf, (int)strlen(stateBuf));
                    devYOffset += g_TextFontSize + 4;

                    snprintf(stateBuf, sizeof(stateBuf), "- Logging: %s (Verbose: %s)", loggingEnabled ? "ON" : "OFF", verboseLogging ? "ON" : "OFF");
                    TextOutA(hdcMem, devX + 15, devYOffset, stateBuf, (int)strlen(stateBuf));
                    devYOffset += g_TextFontSize + 12;

                    // Plugins
                    SetTextColor(hdcMem, RGB(255, 255, 255));
                    TextOutA(hdcMem, devX + 15, devYOffset, "--- LOADED PLUGINS ---", 22);
                    devYOffset += g_TextFontSize + 6;

                    if (fnGetActivePluginsCount) {
                        int pluginsCount = fnGetActivePluginsCount();
                        if (pluginsCount == 0) {
                            SetTextColor(hdcMem, RGB(130, 130, 130));
                            TextOutA(hdcMem, devX + 15, devYOffset, "(No plugins loaded)", 19);
                            devYOffset += g_TextFontSize + 4;
                        } else {
                            for (int p = 0; p < pluginsCount; p++) {
                                wchar_t pName[256] = { 0 };
                                void* baseAddr = nullptr;
                                fnGetActivePluginInfo(p, pName, &baseAddr);

                                char pInfo[128];
                                snprintf(pInfo, sizeof(pInfo), "- %S @ 0x%p", pName, baseAddr);
                                SetTextColor(hdcMem, RGB(200, 200, 200));
                                TextOutA(hdcMem, devX + 15, devYOffset, pInfo, (int)strlen(pInfo));
                                devYOffset += g_TextFontSize + 4;
                            }
                        }
                    } else {
                        SetTextColor(hdcMem, RGB(220, 100, 100));
                        TextOutA(hdcMem, devX + 15, devYOffset, "Plugin API binding missing!", 27);
                        devYOffset += g_TextFontSize + 4;
                    }
                    devYOffset += 8;

                    // Mods
                    SetTextColor(hdcMem, RGB(255, 255, 255));
                    TextOutA(hdcMem, devX + 15, devYOffset, "--- ACTIVE MODS ORDER ---", 25);
                    devYOffset += g_TextFontSize + 6;

                    if (fnGetActiveModsCount) {
                        int modsCount = fnGetActiveModsCount();
                        if (modsCount == 0) {
                            SetTextColor(hdcMem, RGB(130, 130, 130));
                            TextOutA(hdcMem, devX + 15, devYOffset, "(No active mods)", 16);
                            devYOffset += g_TextFontSize + 4;
                        } else {
                            for (int m = 0; m < modsCount; m++) {
                                wchar_t mName[256] = { 0 };
                                fnGetActiveModName(m, mName, 256);

                                char mInfo[128];
                                snprintf(mInfo, sizeof(mInfo), "%d. %S", m + 1, mName);
                                SetTextColor(hdcMem, RGB(200, 200, 200));
                                TextOutA(hdcMem, devX + 15, devYOffset, mInfo, (int)strlen(mInfo));
                                devYOffset += g_TextFontSize + 4;
                                if (devYOffset > devY + devHeight - 20) break;
                            }
                        }
                    } else {
                        SetTextColor(hdcMem, RGB(220, 100, 100));
                        TextOutA(hdcMem, devX + 15, devYOffset, "Mod API binding missing!", 24);
                        devYOffset += g_TextFontSize + 4;
                    }
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

    // Bind Loader Diagnostics functions if available
    HMODULE hLoader = GetModuleHandleA("d3d11.dll");
    if (hLoader) {
        fnGetActiveModsCount = (GetActiveModsCount_t)GetProcAddress(hLoader, "GetActiveModsCount");
        fnGetActiveModName = (GetActiveModName_t)GetProcAddress(hLoader, "GetActiveModName");
        fnGetActivePluginsCount = (GetActivePluginsCount_t)GetProcAddress(hLoader, "GetActivePluginsCount");
        fnGetActivePluginInfo = (GetActivePluginInfo_t)GetProcAddress(hLoader, "GetActivePluginInfo");
        fnGetLoaderDiagnosticInfo = (GetLoaderDiagnosticInfo_t)GetProcAddress(hLoader, "GetLoaderDiagnosticInfo");
        DebugLog("[BattleOverlay] Diagnostics functions successfully bound from d3d11.dll\n");
    } else {
        DebugLog("[BattleOverlay] WARNING: Could not get d3d11.dll module handle for diagnostics binding.\n");
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
        // Check for toggle hotkey: Ctrl + Insert when game is in foreground
        if (GetForegroundWindow() == g_hGameWnd) {
            static bool wasKeyDown = false;
            bool isCtrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            bool isInsertDown = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
            if (isCtrlDown && isInsertDown) {
                if (!wasKeyDown) {
                    g_ShowOverlay = !g_ShowOverlay;
                    ShowWindow(g_hOverlayWnd, g_ShowOverlay ? SW_SHOW : SW_HIDE);
                    wasKeyDown = true;
                }
            } else {
                wasKeyDown = false;
            }

            // Check for developer diagnostics panel toggle hotkey: Ctrl + g_DeveloperHotkey
            if (g_ShowDeveloperPanel) {
                static bool wasDevKeyDown = false;
                bool isDevDown = (GetAsyncKeyState(g_DeveloperHotkey) & 0x8000) != 0;
                if (isCtrlDown && isDevDown) {
                    if (!wasDevKeyDown) {
                        g_DevPanelVisible = !g_DevPanelVisible;
                        wasDevKeyDown = true;
                    }
                } else {
                    wasDevKeyDown = false;
                }
            }
        }

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
