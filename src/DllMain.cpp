#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <thread>

// ============================================================================
// SPEED2.EXE (Need for Speed: Underground 2) static addresses.
// Recovered by static reverse engineering (Ghidra) and cross-checked against
// multiple call sites in the game itself. RVAs are relative to the SPEED2.EXE
// module base (image base 0x400000).
//
// Racer roster (global array, RVA 0x49CCF8):
//   roster[i]           DWORD  car entity pointer for racer slot i
// Built by FUN_005f0d20, which sorts racers by type (local-player class first,
// so roster[0] is the local player's entity) and writes each entity's short
// slot index at entity+0x08.
//
// Car entity (the object in the roster; the CarState sub-structure is embedded
// at entity+0x40, so CarState+0x414..0x420 == entity+0x454..0x460):
//   +0x14        ptr    racer info; (int at racerinfo+4) is the racer type
//   +0x430       float  display speed in m/s - the exact value the HUD
//                        speedometer multiplies by 3.6 (km/h) or 2.237 (mph)
//                        before rounding for the digital readout
//   +0x454       int    nitrous capacity in fixed-point units
//                        (0x7FFFFFFF sentinel = no nitrous / infinite)
//   +0x458       int    nitrous drain per tick while boosting
//   +0x45C       int    nitrous current amount in fixed-point units
//   +0x460       int    boost-active reference (nonzero while boosting)
// Fixed-point scale in the game code is 0.00025 (4000 units = 1 second of
// boost); the game reads/writes these ints through fild/fistp.
//
// Other globals:
//   RVA 0x4384B8  int    pause flag (1 = paused), see FUN_0052cdd0/FUN_0050c680
//   RVA 0x49E7E6  byte   metric units option (nonzero = HUD shows km/h)
// ============================================================================
namespace game {
    constexpr uintptr_t RACER_ROSTER    = 0x49CCF8;
    constexpr uintptr_t PAUSE_FLAG      = 0x4384B8;

    constexpr intptr_t  ENT_RACER_INFO  = 0x14;
    constexpr intptr_t  ENT_SPEED_MS    = 0x430;
    constexpr intptr_t  ENT_NITRO_MAX   = 0x454;
    constexpr intptr_t  ENT_NITRO_CURRENT = 0x45C;
    constexpr intptr_t  ENT_NITRO_BOOSTED = 0x460;

    constexpr int NITRO_NO_SENTINEL = 0x7FFFFFFF;
    constexpr int MAX_RACERS = 22;  // roster parallel arrays are 0x58 (22 slots) apart

    static bool Readable(const void* p, size_t len) {
        return p != nullptr && !IsBadReadPtr(p, len);
    }

    static bool IsGamePaused(uintptr_t base) {
        const int* flag = reinterpret_cast<const int*>(base + PAUSE_FLAG);
        return Readable(flag, 4) && *flag == 1;
    }

    // Resolves the local player's car entity for this frame, or 0.
    // The roster builder puts the local-player class (type 1) racer first.
    static uintptr_t FindPlayerEntity(uintptr_t base) {
        uintptr_t roster = base + RACER_ROSTER;
        if (!Readable(reinterpret_cast<const void*>(roster), 4)) return 0;

        uintptr_t ent = *reinterpret_cast<uintptr_t*>(roster);
        if (ent == 0) return 0;
        if (!Readable(reinterpret_cast<const void*>(ent + ENT_NITRO_CURRENT), 0x10)) return 0;

        // Sanity: the player's racer info should report the local-player type.
        if (Readable(reinterpret_cast<const void*>(ent + ENT_RACER_INFO), 8)) {
            uintptr_t info = *reinterpret_cast<uintptr_t*>(ent + ENT_RACER_INFO);
            if (info != 0 && Readable(reinterpret_cast<const void*>(info + 4), 4) &&
                *reinterpret_cast<int*>(info + 4) != 1) {
                return 0;
            }
        }
        return ent;
    }
}

static bool g_doubleNitrous = false;
static bool g_fastRefill = false;

static void LoadConfig(const std::wstring& iniPath) {
    wchar_t buffer[8] = { 0 };

    DWORD attrs = GetFileAttributesW(iniPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        FILE* f;
        _wfopen_s(&f, iniPath.c_str(), L"w, ccs=UTF-8");
        if (f) {
            fwprintf(f,
                L"; NFSU2 Most Wanted Nitrous System Configuration\n"
                L"; ================================================\n"
                L"\n"
                L"[Settings]\n"
                L"\n"
                L"DoubleNitrous = 0    ; Doubles maximum nitrous capacity for longer use (refills both bars). (0 = Off, 1 = On)\n"
                L"FastRefill    = 0    ; Speeds up nitrous refill and shortens cooldown after use.            (0 = Off, 1 = On)\n"
            );
            fclose(f);
        }
        else {
            g_doubleNitrous = false;
            g_fastRefill = false;
            return;
        }
    }

    GetPrivateProfileStringW(L"Settings", L"DoubleNitrous", L"0", buffer, 8, iniPath.c_str());
    g_doubleNitrous = wcstol(buffer, nullptr, 10) != 0;

    GetPrivateProfileStringW(L"Settings", L"FastRefill", L"0", buffer, 8, iniPath.c_str());
    g_fastRefill = wcstol(buffer, nullptr, 10) != 0;
}

static void NitrousUpdaterThread() {
    HMODULE hGame = GetModuleHandleW(L"SPEED2.exe");
    if (hGame == nullptr) {
        // Ultimate ASI Loader may run us before SPEED2.exe is mapped under that name.
        hGame = GetModuleHandleW(nullptr);
    }
    uintptr_t gameBase = reinterpret_cast<uintptr_t>(hGame);
    if (gameBase == 0) return;

    const int updateIntervalMs = 25;
    const DWORD cooldownMs = g_fastRefill ? 1000 : 2000;
    // The game freezes for roughly half a second right after unpausing.
    // Refilling during that freeze shows up as an instant nitrous jump,
    // so hold the refill for 1s total after unpausing.
    const DWORD unpauseBufferMs = 1000;

    int lastNitroValue = -1;
    ULONGLONG lastDecreaseTime = 0;

    ULONGLONG lastUpdateTime = GetTickCount64();
    ULONGLONG gameTime = 0;
    bool wasGamePaused = false;
    ULONGLONG unpauseTime = 0;

    while (true) {
        ULONGLONG now = GetTickCount64();
        ULONGLONG elapsedTime = now - lastUpdateTime;

        if (elapsedTime < updateIntervalMs) {
            Sleep(static_cast<DWORD>(updateIntervalMs - elapsedTime));
            continue;
        }

        bool isGamePaused = game::IsGamePaused(gameBase);

        if (wasGamePaused && !isGamePaused) {
            unpauseTime = now;
        }

        if (!isGamePaused) {
            if (!wasGamePaused) {
                gameTime += elapsedTime;
            }
        }

        lastUpdateTime = now;
        wasGamePaused = isGamePaused;

        if (isGamePaused) {
            Sleep(updateIntervalMs);
            continue;
        }

        if ((now - unpauseTime) < unpauseBufferMs) {
            Sleep(updateIntervalMs);
            continue;
        }

        uintptr_t ent = game::FindPlayerEntity(gameBase);
        if (ent == 0) {
            lastNitroValue = -1;
            Sleep(updateIntervalMs);
            continue;
        }

        const int maxNitro = *reinterpret_cast<int*>(ent + game::ENT_NITRO_MAX);
        if (maxNitro <= 0 || maxNitro == game::NITRO_NO_SENTINEL) {
            // No nitrous equipped on this car.
            lastNitroValue = -1;
            Sleep(updateIntervalMs);
            continue;
        }

        const uintptr_t nitroAddr = ent + game::ENT_NITRO_CURRENT;
        int currentValue = *reinterpret_cast<int*>(nitroAddr);
        // Allow up to 2x capacity: the DoubleNitrous option overfills past the
        // normal max (the game bleeds the overfill back down itself).
        if (currentValue < 0 || currentValue > maxNitro * 2) {
            lastNitroValue = -1;
            Sleep(updateIntervalMs);
            continue;
        }

        // The game drains this field while boosting; treat any drop (or an
        // active boost, flagged by the game at +0x460) as nitrous usage and
        // restart the refill cooldown.
        const bool boosting = *reinterpret_cast<int*>(ent + game::ENT_NITRO_BOOSTED) != 0;
        if (boosting || (lastNitroValue >= 0 && currentValue < lastNitroValue)) {
            lastDecreaseTime = gameTime;
        }
        lastNitroValue = currentValue;

        // Speed converted to mph. The tuning thresholds below (50-100) were
        // authored for mph, so always use the mph scale regardless of what
        // the HUD displays (the game's km/h option only affects the readout).
        const float speedMs = *reinterpret_cast<float*>(ent + game::ENT_SPEED_MS);
        const int carSpeed = static_cast<int>(speedMs * 2.237f);

        int minSpeed = g_fastRefill ? 20 : 50;
        int maxSpeed = g_fastRefill ? 140 : 100;
        double minTime = g_fastRefill ? 35.0 : 50.0;
        double maxTime = g_fastRefill ? 15.0 : 30.0;

        if (carSpeed < minSpeed) {
            Sleep(updateIntervalMs);
            continue;
        }

        int clampedSpeed = std::max(minSpeed, std::min(carSpeed, maxSpeed));
        double t = (clampedSpeed - minSpeed) / static_cast<double>(maxSpeed - minSpeed);
        double refillTime = minTime + t * (maxTime - minTime);

        // Scale by the real elapsed time so a full bar takes refillTime
        // seconds regardless of how often the update loop actually runs
        // (Sleep granularity makes it ~32 Hz instead of a nominal 40 Hz).
        double dtSec = elapsedTime / 1000.0;
        if (dtSec > 0.25) dtSec = 0.25;  // ignore huge gaps (loading, stalls)
        int refillIncrement = static_cast<int>(maxNitro * dtSec / refillTime);
        if (refillIncrement < 1) refillIncrement = 1;
        int actualMax = g_doubleNitrous ? maxNitro * 2 : maxNitro;

        if ((gameTime - lastDecreaseTime) >= cooldownMs && currentValue < actualMax) {
            *reinterpret_cast<int*>(nitroAddr) = std::min(currentValue + refillIncrement, actualMax);
        }

        Sleep(updateIntervalMs);
    }
}

static DWORD WINAPI MainThread(HMODULE hModule) {
    wchar_t iniPath[MAX_PATH];
    GetModuleFileNameW(hModule, iniPath, MAX_PATH);
    *wcsrchr(iniPath, L'\\') = 0;
    wcscat_s(iniPath, L"\\NFSU2MostWantedNitrousSystem.ini");

    LoadConfig(iniPath);

    std::thread updater(NitrousUpdaterThread);
    updater.detach();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)MainThread, hModule, 0, nullptr);
    }
    return TRUE;
}
