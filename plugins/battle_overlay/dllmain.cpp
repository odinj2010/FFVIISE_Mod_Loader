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
char g_LogPath[MAX_PATH] = { 0 };
char g_LogFile[MAX_PATH] = "battle_overlay_log.txt";

// Definitions of configuration variables
bool g_ShowParty = true;
bool g_ShowEnemies = true;
bool g_ShowATB = true;
bool g_ShowNumericHP = false;
bool g_ShowFPS = false;
bool g_ShowDeveloperPanel = false;
int g_DeveloperHotkey = 0x72;
int g_PanelX = 10;
int g_PanelY = 10;
int g_PanelWidth = 510;
int g_PanelHeight = 410;
COLORREF g_PlayerHPColor = RGB(0, 200, 80);
COLORREF g_EnemyHPColor = RGB(220, 40, 40);
COLORREF g_ATBColor = RGB(255, 140, 0);
COLORREF g_PanelBgColor = RGB(15, 15, 20);
COLORREF g_PanelBorderColor = RGB(0, 122, 204);
COLORREF g_TitleColor = RGB(0, 192, 255);
COLORREF g_TextColorAlly = RGB(230, 230, 230);
COLORREF g_TextColorEnemy = RGB(255, 80, 80);
COLORREF g_TextColorHeader = RGB(255, 255, 255);
char g_FontFace[32] = "Consolas";
int g_TitleFontSize = 20;
int g_TextFontSize = 16;

bool ParseBool(const char* str, bool defaultValue) {
    if (!str || str[0] == '\0') return defaultValue;
    if (_stricmp(str, "true") == 0 || _stricmp(str, "1") == 0 || _stricmp(str, "yes") == 0 || _stricmp(str, "on") == 0) {
        return true;
    }
    if (_stricmp(str, "false") == 0 || _stricmp(str, "0") == 0 || _stricmp(str, "no") == 0 || _stricmp(str, "off") == 0) {
        return false;
    }
    return defaultValue;
}

COLORREF ParseColor(const char* str, COLORREF defaultColor) {
    if (!str || str[0] == '\0') return defaultColor;
    
    // Trim leading whitespace
    while (*str == ' ' || *str == '\t') str++;
    
    // Check if it's hex format like #RRGGBB or 0xRRGGBB
    if (str[0] == '#') {
        unsigned int val = 0;
        if (sscanf_s(str + 1, "%x", &val) == 1) {
            BYTE r = (val >> 16) & 0xFF;
            BYTE g = (val >> 8) & 0xFF;
            BYTE b = val & 0xFF;
            return RGB(r, g, b);
        }
    } else if (strncmp(str, "0x", 2) == 0 || strncmp(str, "0X", 2) == 0) {
        unsigned int val = 0;
        if (sscanf_s(str + 2, "%x", &val) == 1) {
            BYTE r = (val >> 16) & 0xFF;
            BYTE g = (val >> 8) & 0xFF;
            BYTE b = val & 0xFF;
            return RGB(r, g, b);
        }
    } else {
        // Try comma/space separated decimal values e.g. "0, 200, 80"
        int r = 0, g = 0, b = 0;
        if (sscanf_s(str, "%d,%d,%d", &r, &g, &b) == 3 || sscanf_s(str, "%d %d %d", &r, &g, &b) == 3) {
            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;
            return RGB(r, g, b);
        }
    }
    return defaultColor;
}

void LoadConfig() {
    GetModuleFileNameA(g_hModule, g_IniPath, MAX_PATH);
    char* ext = strrchr(g_IniPath, '.');
    if (ext) {
        strcpy_s(ext, sizeof(g_IniPath) - (ext - g_IniPath), ".ini");
    }

    char temp[128];

    // Read EnableLogging (true/false)
    GetPrivateProfileStringA("Config", "EnableLogging", "false", temp, sizeof(temp), g_IniPath);
    g_EnableLogging = ParseBool(temp, false);

    // Read LogFile (string)
    GetPrivateProfileStringA("Config", "LogFile", "battle_overlay_log.txt", g_LogFile, sizeof(g_LogFile), g_IniPath);

    // Resolve full log path in plugins folder
    char logDir[MAX_PATH] = { 0 };
    strcpy_s(logDir, sizeof(logDir), g_IniPath);
    char* lastSlash = strrchr(logDir, '\\');
    if (lastSlash) {
        *(lastSlash + 1) = '\0';
        sprintf_s(g_LogPath, sizeof(g_LogPath), "%s%s", logDir, g_LogFile);
    } else {
        sprintf_s(g_LogPath, sizeof(g_LogPath), "plugins\\%s", g_LogFile);
    }

    g_Opacity = GetPrivateProfileIntA("Config", "Opacity", 102, g_IniPath);
    if (g_Opacity < 0) g_Opacity = 0;
    if (g_Opacity > 255) g_Opacity = 255;

    // Legacy support for HideAllyStats: default is false (meaning ShowParty is true)
    GetPrivateProfileStringA("Config", "HideAllyStats", "false", temp, sizeof(temp), g_IniPath);
    bool hideAlly = ParseBool(temp, false);

    GetPrivateProfileStringA("Config", "ShowParty", hideAlly ? "false" : "true", temp, sizeof(temp), g_IniPath);
    g_ShowParty = ParseBool(temp, true);

    GetPrivateProfileStringA("Config", "ShowEnemies", "true", temp, sizeof(temp), g_IniPath);
    g_ShowEnemies = ParseBool(temp, true);

    GetPrivateProfileStringA("Config", "ShowATB", "true", temp, sizeof(temp), g_IniPath);
    g_ShowATB = ParseBool(temp, true);

    GetPrivateProfileStringA("Config", "ShowNumericHP", "false", temp, sizeof(temp), g_IniPath);
    g_ShowNumericHP = ParseBool(temp, false);

    GetPrivateProfileStringA("Config", "ShowFPS", "false", temp, sizeof(temp), g_IniPath);
    g_ShowFPS = ParseBool(temp, false);

    GetPrivateProfileStringA("Config", "ShowDeveloperPanel", "false", temp, sizeof(temp), g_IniPath);
    g_ShowDeveloperPanel = ParseBool(temp, false);

    g_DeveloperHotkey = GetPrivateProfileIntA("Config", "DeveloperHotkey", 0x72, g_IniPath);

    g_PanelX = GetPrivateProfileIntA("Config", "PanelX", 10, g_IniPath);
    g_PanelY = GetPrivateProfileIntA("Config", "PanelY", 10, g_IniPath);
    g_PanelWidth = GetPrivateProfileIntA("Config", "PanelWidth", 510, g_IniPath);
    g_PanelHeight = GetPrivateProfileIntA("Config", "PanelHeight", 410, g_IniPath);

    g_TitleFontSize = GetPrivateProfileIntA("Config", "TitleFontSize", 20, g_IniPath);
    g_TextFontSize = GetPrivateProfileIntA("Config", "TextFontSize", 16, g_IniPath);

    GetPrivateProfileStringA("Config", "FontFace", "Consolas", g_FontFace, sizeof(g_FontFace), g_IniPath);

    // Read colors
    char colorStr[64];
    GetPrivateProfileStringA("Config", "PlayerHPColor", "0, 200, 80", colorStr, sizeof(colorStr), g_IniPath);
    g_PlayerHPColor = ParseColor(colorStr, RGB(0, 200, 80));

    GetPrivateProfileStringA("Config", "EnemyHPColor", "220, 40, 40", colorStr, sizeof(colorStr), g_IniPath);
    g_EnemyHPColor = ParseColor(colorStr, RGB(220, 40, 40));

    GetPrivateProfileStringA("Config", "ATBColor", "255, 140, 0", colorStr, sizeof(colorStr), g_IniPath);
    g_ATBColor = ParseColor(colorStr, RGB(255, 140, 0));

    GetPrivateProfileStringA("Config", "PanelBgColor", "15, 15, 20", colorStr, sizeof(colorStr), g_IniPath);
    g_PanelBgColor = ParseColor(colorStr, RGB(15, 15, 20));

    GetPrivateProfileStringA("Config", "PanelBorderColor", "0, 122, 204", colorStr, sizeof(colorStr), g_IniPath);
    g_PanelBorderColor = ParseColor(colorStr, RGB(0, 122, 204));

    GetPrivateProfileStringA("Config", "TitleColor", "0, 192, 255", colorStr, sizeof(colorStr), g_IniPath);
    g_TitleColor = ParseColor(colorStr, RGB(0, 192, 255));

    GetPrivateProfileStringA("Config", "TextColorAlly", "230, 230, 230", colorStr, sizeof(colorStr), g_IniPath);
    g_TextColorAlly = ParseColor(colorStr, RGB(230, 230, 230));

    GetPrivateProfileStringA("Config", "TextColorEnemy", "255, 80, 80", colorStr, sizeof(colorStr), g_IniPath);
    g_TextColorEnemy = ParseColor(colorStr, RGB(255, 80, 80));

    GetPrivateProfileStringA("Config", "TextColorHeader", "255, 255, 255", colorStr, sizeof(colorStr), g_IniPath);
    g_TextColorHeader = ParseColor(colorStr, RGB(255, 255, 255));
}

// Background initialization thread
DWORD WINAPI PluginInitThread(LPVOID lpParam) {
    LoadConfig();

    // Clear log file at start if logging is enabled
    if (g_EnableLogging && g_LogPath[0] != '\0') {
        FILE* f = nullptr;
        fopen_s(&f, g_LogPath, "w");
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
