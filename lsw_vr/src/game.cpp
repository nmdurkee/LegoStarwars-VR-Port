#include "game.h"

#include <windows.h>

#include <MinHook.h>

#include <cstring>

#include "log.h"

namespace {
    constexpr uintptr_t kPreferredBase = 0x400000;

    uintptr_t ExeBase() {
        static const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        return base;
    }

    size_t ExeImageSize() {
        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(ExeBase());
        const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(ExeBase() + dos->e_lfanew);
        return nt->OptionalHeader.SizeOfImage;
    }
} // namespace

void *GameAddressRaw(uintptr_t ghidra_address) {
    return reinterpret_cast<void *>(ghidra_address - kPreferredBase + ExeBase());
}

void *GameFunction(const char *name, uintptr_t ghidra_address, const unsigned char *prologue, size_t prologue_size) {
    if (ghidra_address - kPreferredBase + prologue_size > ExeImageSize()) {
        Log("game: %s is outside this exe's image (not the game?)", name);
        return nullptr;
    }
    auto *target = GameAddress<unsigned char>(ghidra_address);
    if (memcmp(target, prologue, prologue_size) != 0) {
        Log("game: %s prologue mismatch at %p (different exe version?)", name, target);
        return nullptr;
    }
    return target;
}

bool HookGameFunction(const char *name, uintptr_t ghidra_address, const unsigned char *prologue,
                      size_t prologue_size, void *detour, void **original) {
    void *target = GameFunction(name, ghidra_address, prologue, prologue_size);
    if (target == nullptr) {
        return false;
    }
    MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status == MH_OK) {
        status = MH_EnableHook(target);
    }
    Log("hook %s %p -> %s", name, target, MH_StatusToString(status));
    return status == MH_OK;
}

namespace {
    volatile DWORD g_game_thread = 0;
} // namespace

void NoteGameThread() {
    g_game_thread = GetCurrentThreadId();
}

bool OnGameThread() {
    return g_game_thread != 0 && g_game_thread == GetCurrentThreadId();
}
