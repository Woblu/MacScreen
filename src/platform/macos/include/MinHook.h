#pragma once

// MinHook API surface used by the shared sources. On macOS every hook is
// registered by name before LWJGL resolves its functions (see
// hooks/symbol_hooks.h), so runtime hook creation reports failure.

#include "platform/macos/win32/win32.h"

typedef enum MH_STATUS {
    MH_UNKNOWN = -1,
    MH_OK = 0,
    MH_ERROR_ALREADY_INITIALIZED,
    MH_ERROR_NOT_INITIALIZED,
    MH_ERROR_ALREADY_CREATED,
    MH_ERROR_NOT_CREATED,
    MH_ERROR_ENABLED,
    MH_ERROR_DISABLED,
    MH_ERROR_NOT_EXECUTABLE,
    MH_ERROR_UNSUPPORTED_FUNCTION,
    MH_ERROR_MEMORY_ALLOC,
    MH_ERROR_MEMORY_PROTECT,
    MH_ERROR_MODULE_NOT_FOUND,
    MH_ERROR_FUNCTION_NOT_FOUND
} MH_STATUS;

#define MH_ALL_HOOKS nullptr

inline MH_STATUS MH_Initialize(void) { return MH_OK; }
inline MH_STATUS MH_CreateHook(LPVOID, LPVOID, LPVOID*) { return MH_ERROR_UNSUPPORTED_FUNCTION; }
inline MH_STATUS MH_EnableHook(LPVOID) { return MH_OK; }
