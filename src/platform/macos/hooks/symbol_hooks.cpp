#include "symbol_hooks.h"

#include "platform/macos/third_party/fishhook/fishhook.h"

#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>

#include <atomic>
#include <cstring>

namespace {

struct HookEntry {
    const char* name;
    void* detour;
    void** original;
};

struct ImportHook {
    const char* imagePrefix;
    const char* symbol;
    void* replacement;
    void** original;
};

constexpr int kMaxHooks = 96;

HookEntry g_dlsymHooks[kMaxHooks];
int g_dlsymHookCount = 0;
HookEntry g_glHooks[kMaxHooks];
int g_glHookCount = 0;
ImportHook g_importHooks[16];
int g_importHookCount = 0;
std::atomic<bool> g_installed{ false };
std::atomic<void*> g_glfwHandle{ nullptr };

const HookEntry* Find(const HookEntry* table, int count, const char* name) {
    if (!name) return nullptr;
    for (int i = 0; i < count; ++i) {
        if (std::strcmp(table[i].name, name) == 0) return &table[i];
    }
    return nullptr;
}

void* Resolve(const HookEntry* entry, void* real) {
    if (!entry || !real) return real;
    if (real == entry->detour) return real;
    if (entry->original) {
        __atomic_store_n(entry->original, real, __ATOMIC_RELEASE);
    }
    return entry->detour;
}

// Calls from this image are bound to the real dlsym/CFBundle functions; only
// liblwjgl's import slots are redirected to these wrappers.
void* HookedDlsym(void* handle, const char* name) {
    void* real = dlsym(handle, name);
    if (real && name && std::strncmp(name, "glfw", 4) == 0 && handle != RTLD_DEFAULT && handle != RTLD_NEXT) {
        g_glfwHandle.store(handle, std::memory_order_release);
    }
    return Resolve(Find(g_dlsymHooks, g_dlsymHookCount, name), real);
}

void* HookedCFBundleGetFunctionPointerForName(CFBundleRef bundle, CFStringRef functionName) {
    void* real = CFBundleGetFunctionPointerForName(bundle, functionName);
    if (!real || !functionName) return real;

    char name[128];
    if (!CFStringGetCString(functionName, name, sizeof(name), kCFStringEncodingASCII)) return real;
    return Resolve(Find(g_glHooks, g_glHookCount, name), real);
}

const char* Basename(const char* path) {
    const char* base = std::strrchr(path, '/');
    return base ? base + 1 : path;
}

bool HasPrefix(const char* path, const char* prefix) {
    return path && std::strncmp(Basename(path), prefix, std::strlen(prefix)) == 0 && std::strstr(Basename(path), ".dylib") != nullptr;
}

void OnImageAdded(const mach_header* header, intptr_t slide) {
    Dl_info info{};
    if (!header || dladdr(header, &info) == 0 || !info.dli_fname) return;

    if (HasPrefix(info.dli_fname, "liblwjgl")) {
        rebinding rebindings[] = {
            { "dlsym", reinterpret_cast<void*>(&HookedDlsym), nullptr },
            { "CFBundleGetFunctionPointerForName", reinterpret_cast<void*>(&HookedCFBundleGetFunctionPointerForName), nullptr },
        };
        rebind_symbols_image(const_cast<mach_header*>(header), slide, rebindings, sizeof(rebindings) / sizeof(rebindings[0]));
    }

    for (int i = 0; i < g_importHookCount; ++i) {
        const ImportHook& hook = g_importHooks[i];
        if (!HasPrefix(info.dli_fname, hook.imagePrefix)) continue;
        if (hook.original && !*hook.original) {
            *hook.original = dlsym(RTLD_DEFAULT, hook.symbol);
        }
        rebinding r = { hook.symbol, hook.replacement, nullptr };
        rebind_symbols_image(const_cast<mach_header*>(header), slide, &r, 1);
    }
}

bool Register(HookEntry* table, int& count, const char* name, void* detour, void** original) {
    if (g_installed.load(std::memory_order_acquire) || !name || !detour || count >= kMaxHooks) return false;
    if (Find(table, count, name)) return false;
    table[count++] = HookEntry{ name, detour, original };
    return true;
}

} // namespace

namespace MacSymbolHooks {

bool RegisterDlsymHook(const char* name, void* detour, void** original) {
    return Register(g_dlsymHooks, g_dlsymHookCount, name, detour, original);
}

bool RegisterGlHook(const char* name, void* detour, void** original) {
    return Register(g_glHooks, g_glHookCount, name, detour, original);
}

bool RegisterImportHook(const char* imagePrefix, const char* symbol, void* replacement, void** original) {
    if (g_installed.load(std::memory_order_acquire) || g_importHookCount >= 16) return false;
    g_importHooks[g_importHookCount++] = ImportHook{ imagePrefix, symbol, replacement, original };
    return true;
}

void Install() {
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
    // Also invoked for every image that is already loaded.
    _dyld_register_func_for_add_image(&OnImageAdded);
}

void* GlfwLibraryHandle() { return g_glfwHandle.load(std::memory_order_acquire); }

void* ResolveGlfw(const char* name) {
    void* handle = g_glfwHandle.load(std::memory_order_acquire);
    return handle ? dlsym(handle, name) : nullptr;
}

} // namespace MacSymbolHooks
