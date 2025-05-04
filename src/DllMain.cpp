#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <vector>
#include <thread>
#include <string>
#include <fstream>
#include <algorithm>

#pragma comment(lib, "psapi.lib")

struct PointerChain {
    DWORD baseOffset;
    std::vector<DWORD> offsets;
};

uintptr_t ResolvePointer(uintptr_t base, const PointerChain& chain) {
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

uintptr_t FindValidNitrousAddress(uintptr_t gameBase, const std::vector<PointerChain>& chains) {
    for (const auto& chain : chains) {
        uintptr_t result = ResolvePointer(gameBase, chain);
        if (result != 0 && !IsBadWritePtr(reinterpret_cast<void*>(result), sizeof(int))) {
            return result;
        }
    }
    return 0;
}

uintptr_t GetModuleBaseAddress(const wchar_t* moduleName) {
    HMODULE hMods[1024];
    HANDLE hProcess = GetCurrentProcess();
    DWORD cbNeeded;
    if (EnumProcessModules(hProcess, hMods, sizeof(hMods), &cbNeeded)) {
        for (unsigned int i = 0; i < (cbNeeded / sizeof(HMODULE)); i++) {
            wchar_t szModName[MAX_PATH];
            if (GetModuleBaseNameW(hProcess, hMods[i], szModName, sizeof(szModName) / sizeof(wchar_t))) {
                if (_wcsicmp(szModName, moduleName) == 0) {
                    return reinterpret_cast<uintptr_t>(hMods[i]);
                }
            }
        }
    }
    return 0;
}

void NitrousUpdaterThread() {
    uintptr_t gameBase = GetModuleBaseAddress(L"SPEED2.exe");
    if (gameBase == 0) return;

    std::vector<PointerChain> nitrousChains = {
        { 0x49CDA8, {0x48, 0x4, 0x20, 0x4, 0xC0, 0x1BC, 0x41C} }
    };

    uintptr_t nitrousAddr = 0;
    Sleep(10000);

    while (true) {
        if (nitrousAddr == 0) {
            nitrousAddr = FindValidNitrousAddress(gameBase, nitrousChains);
            if (nitrousAddr == 0) {
                Sleep(50);
                continue;
            }
        }

        if (!IsBadReadPtr(reinterpret_cast<void*>(nitrousAddr), sizeof(int)) &&
            !IsBadWritePtr(reinterpret_cast<void*>(nitrousAddr), sizeof(int))) {
            int& nitrous = *reinterpret_cast<int*>(nitrousAddr);
            if (nitrous < 24000) {
                nitrous = std::min(nitrous + 25, 24000);
            }
        }
        else {
            nitrousAddr = 0;
        }

        Sleep(50);
    }
}

DWORD WINAPI MainThread(HMODULE hModule) {
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