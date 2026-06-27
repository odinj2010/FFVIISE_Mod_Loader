#pragma once
#include <windows.h>

struct ActorData {
    int current_hp;
    int max_hp;
    int current_mp;
    int max_mp;
    int atb;
    bool stolen;
    bool sensed;
    bool is_active;
};

// Hook related declarations
extern "C" void HookEntry();
extern "C" void HookHandler(uintptr_t rdx, uintptr_t rax);
extern "C" void* g_OriginalInBattlePtr;

extern "C" void HookEntryATB();
extern "C" void HookHandlerATB(uintptr_t rax);
extern "C" void* g_OriginalEnemyNoATB;

// Config settings
extern bool g_EnableLogging;
extern int g_Opacity;
extern char g_IniPath[MAX_PATH];
void LoadConfig();

// Memory scanner API
bool InitializeMemoryScanner();
void CleanupMemoryScanner();
bool IsInBattle();
bool GetActorData(int index, ActorData& outData);
