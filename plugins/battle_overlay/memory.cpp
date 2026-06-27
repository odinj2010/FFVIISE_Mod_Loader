#include "memory.h"
#include <iostream>
#include <vector>
#include "MinHook.h"

// Globals
uintptr_t g_BattlePtr = 0;
void* g_OriginalInBattlePtr = nullptr;
void* g_OriginalEnemyNoATB = nullptr;
bool g_InBattleActive = false;
DWORD g_LastUpdateTick = 0;

// Log function to output to debugger, console, and log file
void DebugLog(const char* format, ...) {
    char buf[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    OutputDebugStringA(buf);

    if (g_EnableLogging) {
        FILE* f = nullptr;
        fopen_s(&f, "plugins\\battle_overlay_log.txt", "a");
        if (f) {
            fprintf(f, "%s", buf);
            fclose(f);
        }
    }
}

// C-linkage handler for the assembly hook
extern "C" void HookHandler(uintptr_t rdx, uintptr_t rax) {
    static int callCount = 0;
    if (callCount < 20) {
        callCount++;
        DebugLog("[BattleOverlay] HookHandler called #%d! rdx: %p, rax: %p, rax&0xFF: %d\n", callCount, (void*)rdx, (void*)rax, (int)(rax & 0xFF));
    }

    if ((rax & 0xFF) == 48) {
        if (g_BattlePtr != rdx) {
            DebugLog("[BattleOverlay] Battle pointer matched and updated! BattlePtr: %p\n", (void*)rdx);
        }
        g_BattlePtr = rdx;
        g_InBattleActive = true;
        g_LastUpdateTick = GetTickCount();
    }
}

// Sanity check to verify a potential BattlePtr base address
bool IsValidBattlePtr(uintptr_t ptr) {
    if (ptr == 0 || (ptr % 8) != 0) return false;
    __try {
        // Read character IDs (2 bytes at the start of actor structures)
        unsigned short id0 = *(unsigned short*)(ptr + 0x100);
        unsigned short id1 = *(unsigned short*)(ptr + 0x168);
        unsigned short id2 = *(unsigned short*)(ptr + 0x1D0);

        // Character 0 (leader) must always be a valid character (Cloud to Cid: 0 to 8)
        if (id0 > 8) return false;

        // Character 1 and 2 must be valid characters (0 to 8) or empty slots (0xFFFF)
        if (id1 > 8 && id1 != 0xFFFF) return false;
        if (id2 > 8 && id2 != 0xFFFF) return false;

        // Read character 0 HP and MP stats
        unsigned int char0_curr_hp = *(unsigned int*)(ptr + 0x108);
        unsigned int char0_max_hp = *(unsigned int*)(ptr + 0x10C);
        unsigned short char0_curr_mp = *(unsigned short*)(ptr + 0x104);
        unsigned short char0_max_mp = *(unsigned short*)(ptr + 0x106);

        // Sanity checks for character 0 (always present in battle)
        if (char0_max_hp >= 100 && char0_max_hp <= 9999 && char0_curr_hp <= char0_max_hp && char0_curr_hp > 0) {
            if (char0_max_mp <= 999 && char0_curr_mp <= char0_max_mp) {
                // If Character 2 (index 1) is active, check its HP too
                if (id1 <= 8) {
                    unsigned int char1_curr_hp = *(unsigned int*)(ptr + 0x170);
                    unsigned int char1_max_hp = *(unsigned int*)(ptr + 0x174);
                    if (char1_max_hp < 100 || char1_max_hp > 9999 || char1_curr_hp > char1_max_hp) {
                        return false;
                    }
                }
                return true;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return false;
}

// C-linkage handler for the ATB assembly hook (runs every frame in battle!)
extern "C" void HookHandlerATB(uintptr_t rax) {
    static int atbCallCount = 0;
    if (atbCallCount < 20) {
        atbCallCount++;
        DebugLog("[BattleOverlay] HookHandlerATB called #%d! rax (ATB Ptr): %p\n", atbCallCount, (void*)rax);
    }

    // ATB_address = BattlePtr - 0x24EC + actor_index * 0x44
    // BattlePtr = ATB_address + 0x24EC - actor_index * 0x44
    for (int i = 0; i < 10; i++) {
        uintptr_t tempPtr = rax + 0x24EC - (i * 0x44);
        if (IsValidBattlePtr(tempPtr)) {
            // Check if there are any live enemies before activating battle state!
            bool hasLiveEnemies = false;
            __try {
                for (int e = 4; e < 10; e++) {
                    uintptr_t actorBase = tempPtr + 0x100 + (e * 0x68);
                    unsigned int enemy_max_hp = *(unsigned int*)(actorBase + 0x0C);
                    unsigned int enemy_curr_hp = *(unsigned int*)(actorBase + 0x08);
                    if (enemy_max_hp > 0 && enemy_curr_hp > 0 && enemy_max_hp <= 999999) {
                        hasLiveEnemies = true;
                        break;
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                hasLiveEnemies = false;
            }

            if (!hasLiveEnemies) {
                // No live enemies, so do not activate/prolong battle state
                continue;
            }

            if (g_BattlePtr != tempPtr) {
                DebugLog("[BattleOverlay] ATB hook resolved BattlePtr: %p (via actor index %d)\n", (void*)tempPtr, i);
            }
            g_BattlePtr = tempPtr;
            g_InBattleActive = true;
            g_LastUpdateTick = GetTickCount();
            break;
        }
    }
}

// Simple pattern scanner
uintptr_t FindPattern(const char* pattern, const char* mask) {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    if (!base) return 0;

    PIMAGE_DOS_HEADER dosHeader = (PIMAGE_DOS_HEADER)base;
    PIMAGE_NT_HEADERS ntHeaders = (PIMAGE_NT_HEADERS)(base + dosHeader->e_lfanew);
    DWORD size = ntHeaders->OptionalHeader.SizeOfImage;

    size_t patternLength = strlen(mask);

    for (DWORD i = 0; i < size - patternLength; i++) {
        bool found = true;
        for (size_t j = 0; j < patternLength; j++) {
            if (mask[j] != '?' && pattern[j] != *(char*)(base + i + j)) {
                found = false;
                break;
            }
        }
        if (found) {
            return base + i;
        }
    }
    return 0;
}

bool InitializeMemoryScanner() {
    DebugLog("[BattleOverlay] Initializing memory scanner...\n");

    // Initialize MinHook library
    MH_STATUS mhStatus = MH_Initialize();
    if (mhStatus != MH_OK && mhStatus != MH_ERROR_ALREADY_INITIALIZED) {
        DebugLog("[BattleOverlay] ERROR: MH_Initialize failed: %d\n", mhStatus);
        return false;
    }

    // Hook 1: InBattlePtr (Sense function)
    const char* pattern1 = "\x0F\xB7\x38\xEB\x02\x8B\xFB\x8B\x0D";
    const char* mask1 = "xxxxxxxxx";
    uintptr_t targetAddr1 = FindPattern(pattern1, mask1);
    if (targetAddr1) {
        DebugLog("[BattleOverlay] Found InBattlePtr signature at: %p\n", (void*)targetAddr1);
        mhStatus = MH_CreateHook((LPVOID)targetAddr1, (LPVOID)&HookEntry, &g_OriginalInBattlePtr);
        if (mhStatus == MH_OK) {
            MH_EnableHook((LPVOID)targetAddr1);
            DebugLog("[BattleOverlay] InBattlePtr hook installed successfully!\n");
        } else {
            DebugLog("[BattleOverlay] WARNING: MH_CreateHook for InBattlePtr failed: %d\n", mhStatus);
        }
    } else {
        DebugLog("[BattleOverlay] WARNING: Failed to locate InBattlePtr signature!\n");
    }

    // Hook 2: EnemyNoATB (ATB update function - runs every frame in battle)
    const char* pattern2 = "\x66\x89\x38\x8B\x0D\xD9\x0A\xEB\x01";
    const char* mask2 = "xxxxxxxxx";
    // We search with a slightly shorter mask to ignore changing displacements if needed,
    // but the pattern "66 89 38 8B 0D" with standard displacement is usually stable.
    uintptr_t targetAddr2 = FindPattern(pattern2, mask2);
    if (targetAddr2) {
        DebugLog("[BattleOverlay] Found EnemyNoATB signature at: %p\n", (void*)targetAddr2);
        mhStatus = MH_CreateHook((LPVOID)targetAddr2, (LPVOID)&HookEntryATB, &g_OriginalEnemyNoATB);
        if (mhStatus == MH_OK) {
            MH_EnableHook((LPVOID)targetAddr2);
            DebugLog("[BattleOverlay] EnemyNoATB hook installed successfully!\n");
        } else {
            DebugLog("[BattleOverlay] WARNING: MH_CreateHook for EnemyNoATB failed: %d\n", mhStatus);
        }
    } else {
        DebugLog("[BattleOverlay] WARNING: Failed to locate EnemyNoATB signature!\n");
    }

    return true;
}

void CleanupMemoryScanner() {
    MH_DisableHook(MH_ALL_HOOKS);
}

bool IsInBattle() {
    if (g_InBattleActive && g_BattlePtr != 0) {
        // Sanity check to see if the pointer still points to active battle actor data
        __try {
            unsigned int max_hp = *(unsigned int*)(g_BattlePtr + 0x10C);
            if (max_hp < 100 || max_hp > 9999) {
                // Max HP is no longer reasonable; battle must have ended
                g_InBattleActive = false;
                g_BattlePtr = 0;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            // Pointer is no longer readable; battle must have ended
            g_InBattleActive = false;
            g_BattlePtr = 0;
        }
    }

    // Check if there are any live enemies on screen
    static DWORD lastSeenLiveEnemyTick = 0;
    if (g_InBattleActive && g_BattlePtr != 0) {
        bool hasLiveEnemies = false;
        __try {
            for (int i = 4; i < 10; i++) {
                uintptr_t actorBase = g_BattlePtr + 0x100 + (i * 0x68);
                unsigned int enemy_max_hp = *(unsigned int*)(actorBase + 0x0C);
                unsigned int enemy_curr_hp = *(unsigned int*)(actorBase + 0x08);
                if (enemy_max_hp > 0 && enemy_curr_hp > 0 && enemy_max_hp <= 999999) {
                    hasLiveEnemies = true;
                    break;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            hasLiveEnemies = false;
        }

        if (hasLiveEnemies) {
            lastSeenLiveEnemyTick = GetTickCount();
        } else {
            // If we have seen no live enemies for more than 1 second, assume battle ended!
            if (GetTickCount() - lastSeenLiveEnemyTick > 1000) {
                g_InBattleActive = false;
                g_BattlePtr = 0;
            }
        }
    } else {
        lastSeenLiveEnemyTick = GetTickCount();
    }

    // Fallback: If we have absolutely no ATB updates for 30 seconds, assume battle ended
    if (g_InBattleActive && (GetTickCount() - g_LastUpdateTick > 30000)) {
        g_InBattleActive = false;
        g_BattlePtr = 0;
    }

    return g_InBattleActive && g_BattlePtr != 0;
}

bool GetActorData(int index, ActorData& outData) {
    if (!IsInBattle() || !g_BattlePtr) {
        return false;
    }

    __try {
        // Read main actor array structure (stride 0x68 starting at offset 0x100)
        uintptr_t actorBase = g_BattlePtr + 0x100 + (index * 0x68);
        
        // Read stats from structure
        outData.current_mp = *(unsigned short*)(actorBase + 0x04);
        outData.max_mp = *(unsigned short*)(actorBase + 0x06);
        outData.current_hp = *(unsigned int*)(actorBase + 0x08);
        outData.max_hp = *(unsigned int*)(actorBase + 0x0C);

        // Read character ID at offset 0x00 of actorBase (2 bytes)
        unsigned short actor_id = *(unsigned short*)(actorBase + 0x00);

        // ATB is at BattlePtr - 0x24EC + index * 0x44
        uintptr_t atbAddress = g_BattlePtr - 0x24EC + (index * 0x44);
        outData.atb = *(unsigned short*)atbAddress;

        // Flags are at BattlePtr - 0x24C7 + index * 0x44
        uintptr_t flagsAddress = g_BattlePtr - 0x24C7 + (index * 0x44);
        unsigned char flags = *(unsigned char*)flagsAddress;

        outData.stolen = (flags & 0x01) != 0;
        outData.sensed = (flags & 0x40) != 0;

        // Determine if actor is active/valid in battle
        // If HP is 0 or Max HP is 0, they are not a valid active slot.
        // For allies (index 0 - 2), character ID must also be a valid party character index (0 - 8).
        if (index < 3) {
            outData.is_active = (outData.max_hp > 0 && actor_id <= 8);
        } else {
            outData.is_active = (outData.max_hp > 0);
        }

        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // Handle potential memory reading access violation safely
        return false;
    }
}
