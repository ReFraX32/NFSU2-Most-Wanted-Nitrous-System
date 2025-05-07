#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <vector>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#pragma comment(lib, "psapi.lib")

struct PointerChain {
    DWORD baseOffset;
    std::vector<DWORD> offsets;
};

static uintptr_t ResolvePointer(uintptr_t base, const PointerChain& chain) {
    uintptr_t addr = base + chain.baseOffset;
    for (size_t i = 0; i < chain.offsets.size(); ++i) {
        if (IsBadReadPtr(reinterpret_cast<void*>(addr), sizeof(uintptr_t))) {
            return 0;
        }
        addr = *reinterpret_cast<uintptr_t*>(addr);
        addr += chain.offsets[i];
    }
    return addr;
}

static uintptr_t GetModuleBaseAddress(const wchar_t* moduleName) {
    HMODULE hMods[1024];
    HANDLE hProcess = GetCurrentProcess();
    DWORD cbNeeded;
    if (EnumProcessModules(hProcess, hMods, sizeof(hMods), &cbNeeded)) {
        for (unsigned int i = 0; i < (cbNeeded / sizeof(HMODULE)); i++) {
            wchar_t szModName[MAX_PATH];
            if (GetModuleBaseNameW(hProcess, hMods[i], szModName, MAX_PATH)) {
                if (_wcsicmp(szModName, moduleName) == 0) {
                    return reinterpret_cast<uintptr_t>(hMods[i]);
                }
            }
        }
    }
    return 0;
}

static bool IsGamePaused(uintptr_t gameBase) {
    uintptr_t pauseFlagAddr = gameBase + 0x4384B8;
    if (IsBadReadPtr(reinterpret_cast<void*>(pauseFlagAddr), sizeof(short))) {
        return false;
    }
    short pauseValue = *reinterpret_cast<short*>(pauseFlagAddr);
    return pauseValue == 1;
}

static void NitrousUpdaterThread() {
    uintptr_t gameBase = GetModuleBaseAddress(L"SPEED2.exe");
    if (gameBase == 0) return;

    std::vector<PointerChain> nitrousChains = {
        { 0x46B2F0, {0x60, 0x4, 0x4, 0x4, 0x4, 0x1BC, 0x41C} },
        { 0x49CDA8, {0x48, 0x4, 0x20, 0x4, 0xC0, 0x1BC, 0x41C} }
    };

    PointerChain speedPointer = { 0x4B4740, {0x114, 0x20, 0x3A8, 0x8, 0xC, 0x20, 0x44} };
    PointerChain maxNitrousPointer = { 0x49CCF8, {0x454} };

    const int updateIntervalMs = 50;
    const DWORD cooldownMs = 2000;

    std::unordered_map<uintptr_t, int> lastValues;
    std::unordered_map<uintptr_t, ULONGLONG> cooldownTimestamps;
    std::unordered_map<uintptr_t, int> maxValues;

    while (true) {
        ULONGLONG now = GetTickCount64();

        if (IsGamePaused(gameBase)) {
            Sleep(updateIntervalMs);
            continue;
        }

        uintptr_t maxNitrousAddr = ResolvePointer(gameBase, maxNitrousPointer);
        if (maxNitrousAddr != 0 && !IsBadReadPtr(reinterpret_cast<void*>(maxNitrousAddr), sizeof(int))) {
            maxValues[0] = *reinterpret_cast<int*>(maxNitrousAddr);
        }

        uintptr_t speedAddr = ResolvePointer(gameBase, speedPointer);
        int carSpeed = (speedAddr != 0 && !IsBadReadPtr(reinterpret_cast<void*>(speedAddr), sizeof(int)))
            ? *reinterpret_cast<int*>(speedAddr)
            : -1;

        double refillTime = 50.0 - ((carSpeed - 50) * 20.0 / 50.0);
        refillTime = std::max(30.0, std::min(refillTime, 50.0));

        int refillIncrement = static_cast<int>(maxValues[0] / (refillTime * 20));

        std::unordered_set<uintptr_t> updatedThisFrame;
        for (const auto& chain : nitrousChains) {
            uintptr_t addr = ResolvePointer(gameBase, chain);
            if (addr == 0 || IsBadReadPtr(reinterpret_cast<void*>(addr), sizeof(int))) {
                continue;
            }

            int currentValue = *reinterpret_cast<int*>(addr);
            if (currentValue < 0 || currentValue > 99999) continue;

            int& lastValue = lastValues[addr];
            ULONGLONG& lastDecreaseTime = cooldownTimestamps[addr];
            int& maxValue = maxValues[0];

            if (currentValue < lastValue) {
                lastDecreaseTime = now;
            }

            lastValue = currentValue;

            if ((now - lastDecreaseTime) < cooldownMs) continue;
            if (carSpeed >= 0 && carSpeed < 50) continue;

            bool alreadyHandled = false;
            for (uintptr_t seenAddr : updatedThisFrame) {
                if (*reinterpret_cast<int*>(seenAddr) == currentValue) {
                    alreadyHandled = true;
                    break;
                }
            }

            if (!alreadyHandled && currentValue < maxValue) {
                *reinterpret_cast<int*>(addr) = std::min(currentValue + refillIncrement, maxValue);
                updatedThisFrame.insert(addr);
            }
        }

        Sleep(updateIntervalMs);
    }
}

static DWORD WINAPI MainThread(HMODULE hModule) {
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