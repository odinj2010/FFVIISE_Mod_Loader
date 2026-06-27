#pragma once
#include <windows.h>

bool InitializeOverlay();
void CleanupOverlay();
void StartOverlayThread();
void StopOverlayThread();
