#pragma once

// macOS replacement for the MinHook-based hooks used on Windows.
//
// LWJGL resolves every GLFW/SDL function with dlsym() and every OpenGL function
// with CFBundleGetFunctionPointerForName(), both called from liblwjgl.dylib.
// Rebinding those two imports inside LWJGL's libraries (and nowhere else) lets us
// hand LWJGL a detour for any function by name without patching code pages.
namespace MacSymbolHooks {

// Hooks for symbols LWJGL resolves with dlsym (GLFW).
// `original` receives the real address whenever LWJGL resolves the name.
bool RegisterDlsymHook(const char* name, void* detour, void** original);

// Hooks for OpenGL entry points LWJGL resolves through the OpenGL bundle.
bool RegisterGlHook(const char* name, void* detour, void** original);

// Rebinds an imported symbol inside every loaded image whose file name starts
// with `imagePrefix` (e.g. "libglfw"). `original` receives the real address.
bool RegisterImportHook(const char* imagePrefix, const char* symbol, void* replacement, void** original);

// Starts watching for liblwjgl / hooked images. Call once after registering hooks.
void Install();

// Resolves a GLFW function from the library LWJGL loaded.
void* ResolveGlfw(const char* name);

} // namespace MacSymbolHooks
