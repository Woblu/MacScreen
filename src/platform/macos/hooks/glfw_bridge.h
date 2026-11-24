#pragma once

#include "platform/macos/win32/win32.h"

// Connects Minecraft's GLFW window to the emulated Win32 window.
//
// On Windows the chain is: OS -> Toolscreen's subclassed WndProc -> GLFW's
// WndProc -> Minecraft's GLFW callbacks. Here GLFW's Cocoa events are turned
// into the same window messages, run through Toolscreen's WndProc unchanged,
// and whatever Toolscreen forwards is turned back into GLFW callbacks for the
// game by GlfwWndProc (the stand-in for GLFW's Win32 window procedure).
namespace MacGlfwBridge {

using WindowSizeFun = void (*)(void* window, int width, int height);
using SetSizeCallbackFun = WindowSizeFun (*)(void* window, WindowSizeFun callback);

// Registers the bridge's own dlsym hooks (input callbacks, input getters,
// event polling). Upstream's GLFW detours are registered by the dllmain glue.
void RegisterHooks();

// --- functions installed as upstream's "original" GLFW pointers ---
void* CreateWindowThroughBridge(int width, int height, const char* title, void* monitor, void* share);
void DestroyWindowThroughBridge(void* window);
void SetInputModeThroughBridge(void* window, int mode, int value);
void SetCursorThroughBridge(void* window, void* cursor);
WindowSizeFun SetWindowSizeCallbackThroughBridge(void* window, WindowSizeFun callback);
WindowSizeFun SetFramebufferSizeCallbackThroughBridge(void* window, WindowSizeFun callback);

// Upstream's size-callback hooks; LWJGL's calls are routed through them with
// the bridge's trampolines so Toolscreen captures callbacks in pixel units.
void SetUpstreamSizeCallbackSetters(SetSizeCallbackFun windowSize, SetSizeCallbackFun framebufferSize);

// Real entry points filled in when LWJGL resolves them.
void** RealCreateWindowSlot();
void** RealDestroyWindowSlot();
void** RealSetInputModeSlot();
void** RealSetCursorSlot();
void** RealSwapBuffersSlot();

// --- frame ---
// Main thread, before Toolscreen renders: refreshes window state and the
// cursor clip.
void BeforeSwap(void* window);
bool RealFramebufferSize(void* window, int& width, int& height);
void RealSwapBuffers(void* window);

// --- upstream services the bridge calls ---
void SetSensitivityResolver(void (*resolver)(float& x, float& y));
// Upstream's SetCursorPos hook; GLFW's own cursor warps are routed through it.
void SetCursorPosHook(BOOL (*hook)(int x, int y));
// Real cursor warp in screen pixels (installed as upstream's oSetCursorPos).
BOOL WarpCursorScreen(int x, int y);
void SetGuiOpenQuery(bool (*query)());

} // namespace MacGlfwBridge
