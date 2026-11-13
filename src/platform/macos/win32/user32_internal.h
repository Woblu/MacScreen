#pragma once

// Window, message and input-state emulation shared between the Win32 layer
// (user32.cpp) and the GLFW bridge that feeds it.

#include "platform/macos/win32/win32.h"

#include <functional>

namespace MacWin32 {

// --- the game window ----------------------------------------------------------------

// Registers the GLFW window as an HWND. `defaultProc` plays the part of GLFW's
// own window procedure and receives whatever Toolscreen forwards to the game.
HWND RegisterGameWindow(void* glfwWindow, void* nsWindow, WNDPROC defaultProc);
void UnregisterGameWindow(HWND hwnd);
HWND GameWindow();
bool IsGameWindow(HWND hwnd);
void* NativeWindow(HWND hwnd);

struct WindowSnapshot {
    RECT clientScreen{};  // client area in screen pixels
    RECT frameScreen{};   // window frame in screen pixels
    int clientWidth = 0;  // framebuffer pixels
    int clientHeight = 0;
    double scale = 1.0;   // backing scale (pixels per point)
    bool key = false;
    bool visible = true;
    bool minimized = false;
    bool zoomed = false;
    bool fullscreen = false;
    bool borderless = false;
    std::string title;
};
// Main thread only; other threads read the latest snapshot.
void UpdateWindowSnapshot(HWND hwnd, const WindowSnapshot& snapshot);
WindowSnapshot GetWindowSnapshot(HWND hwnd);

// Delivers a message to the window's current window procedure with Win32
// message context (GetMessageExtraInfo / GetMessageTime).
LRESULT DispatchToWindow(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LPARAM extraInfo = 0);

// Delivers posted/sent messages, due timers and injected input. Main thread.
void PumpMessages();
// True while a message is being delivered on this thread.
bool InDispatch();

// --- keyboard state ---------------------------------------------------------------------

// Updates GetAsyncKeyState/GetKeyState/GetKeyboardState for a sided VK
// (VK_LSHIFT, ...) or mouse button VK. Generic modifiers follow automatically.
void SetKeyDown(unsigned vk, bool down);
void ReleaseAllKeys();
bool IsKeyDown(unsigned vk);

// Runs WH_KEYBOARD_LL hooks. Returns true if a hook swallowed the event.
bool RunLowLevelKeyboardHooks(WPARAM msg, const KBDLLHOOKSTRUCT& info);
bool HasLowLevelKeyboardHooks();

// SendInput events are handed to this callback from PumpMessages.
void SetInputInjector(std::function<void(const INPUT&)> injector);

// --- cursor -------------------------------------------------------------------------------

struct CursorCallbacks {
    // Win32 visibility (ShowCursor counter, SetCursor(NULL)) changed.
    std::function<void(bool visible)> visibilityChanged;
    // True when the game itself hides the cursor (GLFW disabled/hidden mode).
    std::function<bool()> gameHidesCursor;
    // SetCursorPos target in screen pixels.
    std::function<void(int x, int y)> warp;
};
void SetCursorCallbacks(CursorCallbacks callbacks);
bool IsWin32CursorVisible();
// Cursor last set with SetCursor (nullptr = hidden).
HCURSOR CurrentCursor();
// Re-applies the current cursor image (after GLFW reset it).
void ReapplyCursor();
// ClipCursor rectangle in screen pixels, if any.
bool GetCursorClip(RECT& rect);
// Mouse capture / leave tracking for TrackMouseEvent.
bool ConsumeMouseLeaveRequest(HWND hwnd);

// Thread id of the game's window thread (main thread).
DWORD WindowThreadId();

// CGWindowID behind an HWND that refers to another application's window.
bool ForeignWindowId(HWND hwnd, uint32_t& windowId);

} // namespace MacWin32
