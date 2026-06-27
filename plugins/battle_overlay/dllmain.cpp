#include <windows.h>
#include <stdio.h>
#include "memory.h"
#include "overlay.h"

void DebugLog(const char* format, ...);

// Config variables definitions
HMODULE g_hModule = NULL;
bool g_EnableLogging = false;
int g_Opacity = 102;
char g_IniPath[MAX_PATH] = { 0 };

void LoadConfig() {
    GetModuleFileNameA(g_hModule, g_IniPath, MAX_PATH);
    char* ext = strrchr(g_IniPath, '.');
    if (ext) {
        strcpy_s(ext, sizeof(g_IniPath) - (ext - g_IniPath), ".ini");
    }

    // Read config settings
    g_EnableLogging = GetPrivateProfileIntA("Config", "EnableLogging", 0, g_IniPath) != 0;
    g_Opacity = GetPrivateProfileIntA("Config", "Opacity", 102, g_IniPath);
    if (g_Opacity < 0) g_Opacity = 0;
    if (g_Opacity > 255) g_Opacity = 255;
}

// Background initialization thread
DWORD WINAPI PluginInitThread(LPVOID lpParam) {
    LoadConfig();

    // Clear log file at start if logging is enabled
    if (g_EnableLogging) {
        FILE* f = nullptr;
        fopen_s(&f, "plugins\\battle_overlay_log.txt", "w");
        if (f) fclose(f);
    }

    DebugLog("[BattleOverlay] Starting initialization...\n");

    // Delay initialization slightly to let the game fully startup
    Sleep(2000);

    // Initialize Memory Scanner
    if (!InitializeMemoryScanner()) {
        DebugLog("[BattleOverlay] Failed to initialize Memory Scanner!\n");
        return 1;
    }

    // Start Overlay rendering thread
    StartOverlayThread();
    
    DebugLog("[BattleOverlay] Plugin initialized successfully!\n");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            g_hModule = hModule;
            DisableThreadLibraryCalls(hModule);
            CreateThread(NULL, 0, PluginInitThread, NULL, 0, NULL);
            break;
        case DLL_PROCESS_DETACH:
            StopOverlayThread();
            CleanupMemoryScanner();
            break;
    }
    return TRUE;
}
