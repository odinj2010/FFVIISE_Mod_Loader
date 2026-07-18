#pragma once
#include <windows.h>

bool InitializeOverlay();
void CleanupOverlay();
void StartOverlayThread();
void StopOverlayThread();

// Configuration Settings
extern bool g_ShowParty;
extern bool g_ShowEnemies;
extern bool g_ShowATB;
extern bool g_ShowNumericHP;
extern int g_PanelX;
extern int g_PanelY;
extern int g_PanelWidth;
extern int g_PanelHeight;
extern COLORREF g_PlayerHPColor;
extern COLORREF g_EnemyHPColor;
extern COLORREF g_ATBColor;
extern COLORREF g_PanelBgColor;
extern COLORREF g_PanelBorderColor;
extern COLORREF g_TitleColor;
extern COLORREF g_TextColorAlly;
extern COLORREF g_TextColorEnemy;
extern COLORREF g_TextColorHeader;
extern char g_FontFace[32];
extern int g_TitleFontSize;
extern int g_TextFontSize;
extern bool g_ShowFPS;
