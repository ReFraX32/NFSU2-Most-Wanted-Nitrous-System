#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <vector>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <string>

#pragma comment(lib, "psapi.lib")

struct PointerChain {
    DWORD baseOffset;
    std::vector<DWORD> offsets;
};

static bool g_doubleNitrous = false;
static bool g_fastRefill = false;

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
    uintptr_t gameBase = GetModuleBaseAddress(L"SPEED2.exe");
    if (gameBase == 0) return;

    std::vector<PointerChain> nitrousChains = {
        { 0x46B2F0, {0x60, 0x4, 0x4, 0x4, 0x4, 0x1BC, 0x41C} },
        { 0x49CDA8, {0x48, 0x4, 0x20, 0x4, 0xC0, 0x1BC, 0x41C} }
    };

    PointerChain speedPointer = { 0x4B4740, {0x114, 0x20, 0x3A8, 0x8, 0xC, 0x20, 0x44} };
    PointerChain maxNitrousPointer = { 0x49CCF8, {0x454} };

    const int updateIntervalMs = 25;
    const DWORD cooldownMs = g_fastRefill ? 1000 : 2000;

    std::unordered_map<uintptr_t, int> lastValues;
    std::unordered_map<uintptr_t, ULONGLONG> cooldownTimestamps;
    std::unordered_map<uintptr_t, int> maxValues;

    ULONGLONG lastUpdateTime = GetTickCount64();

    while (true) {
        ULONGLONG now = GetTickCount64();
        ULONGLONG elapsedTime = now - lastUpdateTime;

        if (elapsedTime < updateIntervalMs) {
            Sleep(static_cast<DWORD>(updateIntervalMs - elapsedTime));
            continue;
        }

        lastUpdateTime = now;

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

        int baseMax = maxValues[0];
        int refillIncrement = static_cast<int>(baseMax / (refillTime * 40));
        int actualMax = g_doubleNitrous ? baseMax * 2 : baseMax;

        std::unordered_set<uintptr_t> updatedThisFrame;
        for (const auto& chain : nitrousChains) {
            uintptr_t addr = ResolvePointer(gameBase, chain);
            if (addr == 0 || IsBadReadPtr(reinterpret_cast<void*>(addr), sizeof(int))) continue;

            int currentValue = *reinterpret_cast<int*>(addr);
            if (currentValue < 0 || currentValue > 99999) continue;

            int& lastValue = lastValues[addr];
            ULONGLONG& lastDecreaseTime = cooldownTimestamps[addr];

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

            if (!alreadyHandled && currentValue < actualMax) {
                *reinterpret_cast<int*>(addr) = std::min(currentValue + refillIncrement, actualMax);
                updatedThisFrame.insert(addr);
            }
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