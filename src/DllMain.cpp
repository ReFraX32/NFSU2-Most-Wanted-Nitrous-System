#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <vector>
#include <thread>
#include <string>
#include <fstream>
#include <algorithm>
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

static void NitrousUpdaterThread() {
    uintptr_t gameBase = GetModuleBaseAddress(L"SPEED2.exe");
    if (gameBase == 0) return;

    std::vector<PointerChain> nitrousChains = {
        { 0x46B2F0, {0x60, 0x4, 0x4, 0x4, 0x4, 0x1BC, 0x41C} },
        { 0x46B2F0, {0x34, 0x18, 0x8, 0x1A0, 0x4, 0x1BC, 0x41C} },
        { 0x46B2F0, {0x54, 0x4, 0xC4, 0x1A0, 0x4, 0x1BC, 0x41C} },
        { 0x49CDA8, {0x48, 0x4, 0x20, 0x4, 0xC0, 0x1BC, 0x41C} },
        { 0x49CCF8, {0x34, 0x44, 0x4, 0x14, 0x8, 0x1BC, 0x41C} }
    };

    Sleep(10000);

    const int incrementAmount = 15;
    const int updateIntervalMs = 50;
    const DWORD cooldownMs = 2000;

    std::unordered_map<uintptr_t, int> lastValues;
    std::unordered_map<uintptr_t, ULONGLONG> cooldownTimestamps;
    std::unordered_map<uintptr_t, int> maxValues;
    std::unordered_map<uintptr_t, ULONGLONG> firstSeenTimestamps;

    while (true) {
        ULONGLONG now = GetTickCount64();
        std::unordered_set<uintptr_t> updatedThisFrame;

        for (const auto& chain : nitrousChains) {
            uintptr_t addr = ResolvePointer(gameBase, chain);
            if (addr == 0 ||
                IsBadReadPtr(reinterpret_cast<void*>(addr), sizeof(int)) ||
                IsBadWritePtr(reinterpret_cast<void*>(addr), sizeof(int))) {
                continue;
            }

            int currentValue = *reinterpret_cast<int*>(addr);
            if (currentValue < 0 || currentValue > 30000) continue;

            int& lastValue = lastValues[addr];
            ULONGLONG& lastDecreaseTime = cooldownTimestamps[addr];
            ULONGLONG& firstSeen = firstSeenTimestamps[addr];
            int& maxValue = maxValues[addr];

            if (firstSeen == 0) {
                firstSeen = now;
                maxValue = 0;
            }

            if ((now - firstSeen) < 10) {
                if (currentValue >= 1000 && currentValue <= 30000 && currentValue > maxValue) {
                    maxValue = currentValue;
                }
            }

            if (currentValue < lastValue) {
                lastDecreaseTime = now;
            }
            lastValue = currentValue;

            if ((now - lastDecreaseTime) < cooldownMs) continue;

            bool alreadyHandled = false;
            for (uintptr_t seenAddr : updatedThisFrame) {
                if (*reinterpret_cast<int*>(seenAddr) == currentValue) {
                    alreadyHandled = true;
                    break;
                }
            }

            if (!alreadyHandled && currentValue < maxValue) {
                *reinterpret_cast<int*>(addr) = std::min(currentValue + incrementAmount, maxValue);
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