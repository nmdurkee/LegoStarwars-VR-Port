#pragma once

#include <cstddef>
#include <cstdint>

// Access to LEGOStarWarsSaga.exe (PC, Steam) code and data by the addresses
// found in Ghidra (preferred image base 0x400000).

// Pointer to a game address, rebased to where the exe is loaded.
void *GameAddressRaw(uintptr_t ghidra_address);

template <typename T> T *GameAddress(uintptr_t ghidra_address) {
    return static_cast<T *>(GameAddressRaw(ghidra_address));
}

// Hooks a game function with MinHook if its first bytes match the expected
// prologue (so a different exe version is skipped instead of crashed) and it
// lies inside the exe image (the proxy can be loaded by other programs).
// Requires MinHook to be initialized. Logs the outcome.
bool HookGameFunction(const char *name, uintptr_t ghidra_address, const unsigned char *prologue,
                      size_t prologue_size, void *detour, void **original);

// Address of a game function to call, or nullptr (logged) if its first bytes
// don't match the expected prologue or it is outside the exe image.
void *GameFunction(const char *name, uintptr_t ghidra_address, const unsigned char *prologue, size_t prologue_size);

template <typename Fn, size_t N>
Fn GameFunction(const char *name, uintptr_t ghidra_address, const unsigned char (&prologue)[N]) {
    return reinterpret_cast<Fn>(GameFunction(name, ghidra_address, prologue, N));
}

// The game's own (update/render) thread: noted from inside game hooks, so code
// running elsewhere (Present) can check before calling game functions.
void NoteGameThread();
bool OnGameThread();
