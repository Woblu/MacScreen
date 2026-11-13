#include "platform/macos/hooks/glfw_bridge.h"

#include "platform/macos/hooks/symbol_hooks.h"
#include "platform/macos/os/mac_os.h"
#include "platform/macos/win32/gdi_internal.h"
#include "platform/macos/win32/keymap.h"
#include "platform/macos/win32/user32_internal.h"
#include "platform/macos/win32/win32_internal.h"

#include "hooks/input_hook.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

namespace {

// --- GLFW constants ------------------------------------------------------------------
constexpr int kRelease = 0;
constexpr int kPress = 1;
constexpr int kRepeat = 2;
constexpr int kModShift = 0x0001;
constexpr int kModControl = 0x0002;
constexpr int kModAlt = 0x0004;
constexpr int kModSuper = 0x0008;
constexpr int kModCapsLock = 0x0010;
constexpr int kModNumLock = 0x0020;
constexpr int kCursor = 0x00033001;
constexpr int kLockKeyMods = 0x00033004;
constexpr int kCursorNormal = 0x00034001;
constexpr int kCursorDisabled = 0x00034003;
constexpr int kKeyLast = 348;
constexpr int kMouseButtonLast = 7;
constexpr int kKeyCapsLock = 280;
constexpr int kKeyLeftShift = 340;
constexpr int kKeyLeftControl = 341;
constexpr int kKeyLeftAlt = 342;
constexpr int kKeyLeftSuper = 343;
constexpr int kKeyRightShift = 344;
constexpr int kKeyRightControl = 345;
constexpr int kKeyRightAlt = 346;
constexpr int kKeyRightSuper = 347;

using KeyFun = void (*)(void*, int, int, int, int);
using CharFun = void (*)(void*, unsigned int);
using CharModsFun = void (*)(void*, unsigned int, int);
using MouseButtonFun = void (*)(void*, int, int, int);
using CursorPosFun = void (*)(void*, double, double);
using ScrollFun = void (*)(void*, double, double);
using FocusFun = void (*)(void*, int);
using EnterFun = void (*)(void*, int);
using SizeFun = MacGlfwBridge::WindowSizeFun;

// --- real GLFW entry points ----------------------------------------------------------------
struct Real {
    void* (*createWindow)(int, int, const char*, void*, void*) = nullptr;
    void (*destroyWindow)(void*) = nullptr;
    void (*windowHint)(int, int) = nullptr;
    void (*swapBuffers)(void*) = nullptr;
    void (*pollEvents)() = nullptr;
    void (*waitEvents)() = nullptr;
    void (*waitEventsTimeout)(double) = nullptr;
    void (*setInputMode)(void*, int, int) = nullptr;
    int (*getInputMode)(void*, int) = nullptr;
    void (*setCursor)(void*, void*) = nullptr;
    KeyFun (*setKeyCallback)(void*, KeyFun) = nullptr;
    CharFun (*setCharCallback)(void*, CharFun) = nullptr;
    CharModsFun (*setCharModsCallback)(void*, CharModsFun) = nullptr;
    MouseButtonFun (*setMouseButtonCallback)(void*, MouseButtonFun) = nullptr;
    CursorPosFun (*setCursorPosCallback)(void*, CursorPosFun) = nullptr;
    ScrollFun (*setScrollCallback)(void*, ScrollFun) = nullptr;
    FocusFun (*setWindowFocusCallback)(void*, FocusFun) = nullptr;
    EnterFun (*setCursorEnterCallback)(void*, EnterFun) = nullptr;
    SizeFun (*setWindowSizeCallback)(void*, SizeFun) = nullptr;
    SizeFun (*setFramebufferSizeCallback)(void*, SizeFun) = nullptr;
    int (*getKey)(void*, int) = nullptr;
    int (*getMouseButton)(void*, int) = nullptr;
    void (*getCursorPos)(void*, double*, double*) = nullptr;
    void (*setCursorPos)(void*, double, double) = nullptr;
    void (*getFramebufferSize)(void*, int*, int*) = nullptr;
    void* (*getCocoaWindow)(void*) = nullptr;
    void (*warpCursor)(double, double) = nullptr; // CGWarpMouseCursorPosition(CGPoint) as two doubles
    int (*moveCursorToPoint)(uint32_t, double, double) = nullptr;
};
Real g_real;

template <typename T> T ResolveLazy(T& slot, const char* name) {
    if (!slot) slot = reinterpret_cast<T>(MacSymbolHooks::ResolveGlfw(name));
    return slot;
}

// --- the game's GLFW callbacks ---------------------------------------------------------------
struct GameCallbacks {
    KeyFun key = nullptr;
    CharFun character = nullptr;
    CharModsFun charMods = nullptr;
    MouseButtonFun mouseButton = nullptr;
    CursorPosFun cursorPos = nullptr;
    ScrollFun scroll = nullptr;
    FocusFun focus = nullptr;
    EnterFun enter = nullptr;
    SizeFun windowSize = nullptr;
    SizeFun framebufferSize = nullptr;
};

struct BridgeState {
    void* window = nullptr;
    void* nsWindow = nullptr;
    HWND hwnd = nullptr;
    GameCallbacks game;

    // What the game has seen (rebinds and blocks applied).
    bool gameKeys[kKeyLast + 1] = {};
    bool gameButtons[kMouseButtonLast + 1] = {};
    double gameCursorX = 0.0;
    double gameCursorY = 0.0;
    bool lockKeyMods = false;

    // Cursor mode the game requested and the bridge applied.
    int cursorMode = kCursorNormal;
    void* gameCursor = nullptr;
    double lastRawX = 0.0;
    double lastRawY = 0.0;
    bool haveRaw = false;
    bool forcedVisible = false;
    bool focusChanged = false;

    // Last keystroke lParam for WM_CHAR.
    LPARAM lastKeyLParam = 1;
    double scrollRemainderY = 0.0;
    double scrollRemainderX = 0.0;
    WCHAR pendingHighSurrogate = 0;
    int titleCountdown = 0;
};

std::recursive_mutex g_stateMutex;
BridgeState g_state;

void (*g_sensitivityResolver)(float&, float&) = nullptr;
BOOL (*g_cursorPosHook)(int, int) = nullptr;
bool (*g_guiOpenQuery)() = nullptr;
MacGlfwBridge::SetSizeCallbackFun g_upstreamSetWindowSize = nullptr;
MacGlfwBridge::SetSizeCallbackFun g_upstreamSetFramebufferSize = nullptr;
thread_local int t_bridgeWarpDepth = 0;

double Scale() {
    const double s = MacWin32::GetWindowSnapshot(g_state.hwnd).scale;
    return s > 0.0 ? s : 1.0;
}

int GameMods() {
    int mods = 0;
    const bool* k = g_state.gameKeys;
    if (k[kKeyLeftShift] || k[kKeyRightShift]) mods |= kModShift;
    if (k[kKeyLeftControl] || k[kKeyRightControl]) mods |= kModControl;
    if (k[kKeyLeftAlt] || k[kKeyRightAlt]) mods |= kModAlt;
    if (k[kKeyLeftSuper] || k[kKeyRightSuper]) mods |= kModSuper;
    if (g_state.lockKeyMods) {
        if (GetKeyState(VK_CAPITAL) & 1) mods |= kModCapsLock;
        mods |= kModNumLock;
    }
    return mods;
}

WPARAM MouseKeyFlags() {
    WPARAM flags = 0;
    if (MacWin32::IsKeyDown(VK_LBUTTON)) flags |= MK_LBUTTON;
    if (MacWin32::IsKeyDown(VK_RBUTTON)) flags |= MK_RBUTTON;
    if (MacWin32::IsKeyDown(VK_MBUTTON)) flags |= MK_MBUTTON;
    if (MacWin32::IsKeyDown(VK_XBUTTON1)) flags |= MK_XBUTTON1;
    if (MacWin32::IsKeyDown(VK_XBUTTON2)) flags |= MK_XBUTTON2;
    if (MacWin32::IsKeyDown(VK_SHIFT)) flags |= MK_SHIFT;
    if (MacWin32::IsKeyDown(VK_CONTROL)) flags |= MK_CONTROL;
    return flags;
}

bool GameHidesCursor() { return g_state.cursorMode != kCursorNormal; }

bool GuiOpen() { return g_guiOpenQuery && g_guiOpenQuery(); }

// Delivers a message and then everything Toolscreen posted or injected while
// handling it, matching how Windows retrieves posted messages before input.
void Deliver(UINT msg, WPARAM wParam, LPARAM lParam, LPARAM extraInfo = 0) {
    if (!g_state.hwnd) return;
    MacWin32::DispatchToWindow(g_state.hwnd, msg, wParam, lParam, extraInfo);
    if (!MacWin32::InDispatch()) MacWin32::PumpMessages();
}

// --- keyboard -----------------------------------------------------------------------------------

void DeliverKey(int macKeyCode, unsigned vk, unsigned scan, int action, LPARAM extraInfo, bool injected) {
    const bool down = action != kRelease;
    const bool wasDown = MacWin32::IsKeyDown(vk);
    const bool altDown = MacWin32::IsKeyDown(VK_MENU) || (down && (vk == VK_LMENU || vk == VK_RMENU));
    const bool isAltKey = vk == VK_LMENU || vk == VK_RMENU || vk == VK_MENU;
    const bool system = altDown || isAltKey || vk == VK_F10;
    const UINT msg = down ? (system ? WM_SYSKEYDOWN : WM_KEYDOWN) : (system ? WM_SYSKEYUP : WM_KEYUP);
    const bool extended = (scan & MacKeymap::kExtended) != 0;

    if (MacWin32::HasLowLevelKeyboardHooks()) {
        KBDLLHOOKSTRUCT info{};
        info.vkCode = vk;
        info.scanCode = scan & 0xFF;
        info.flags = (extended ? LLKHF_EXTENDED : 0) | (down ? 0 : LLKHF_UP) | (altDown ? LLKHF_ALTDOWN : 0) |
                     (injected ? LLKHF_INJECTED : 0);
        info.time = GetTickCount();
        info.dwExtraInfo = static_cast<ULONG_PTR>(extraInfo);
        if (MacWin32::RunLowLevelKeyboardHooks(msg, info)) return;
    }

    MacWin32::SetKeyDown(vk, down);

    LPARAM lParam = 1 | (static_cast<LPARAM>(scan & 0xFF) << 16);
    if (extended) lParam |= (1 << 24);
    if (altDown && system) lParam |= (1 << 29);
    if (wasDown || action == kRepeat || !down) lParam |= (1 << 30);
    if (!down) lParam |= static_cast<LPARAM>(1u << 31);
    if (down) g_state.lastKeyLParam = lParam;
    (void)macKeyCode;

    Deliver(msg, MacKeymap::GenericVk(vk), lParam, extraInfo);
}

void KeyCallback(void* window, int key, int scancode, int action, int mods) {
    (void)mods;
    std::lock_guard<std::recursive_mutex> lock(g_stateMutex);
    if (window != g_state.window) return;

    unsigned vk = MacKeymap::VkFromMacKeyCode(scancode);
    unsigned scan = MacKeymap::ScanCodeFromMacKeyCode(scancode);
    if (vk == 0 || scan == 0) {
        // Keys Windows has no equivalent for go straight to the game.
        if (g_state.game.key && key >= 0 && key <= kKeyLast) {
            g_state.gameKeys[key] = action != kRelease;
            g_state.game.key(window, key, scancode, action, GameMods());
        }
        return;
    }

    if (key == kKeyCapsLock) {
        // Cocoa reports the lock state rather than the key; every change is a press.
        DeliverKey(scancode, vk, scan, kPress, 0, false);
        DeliverKey(scancode, vk, scan, kRelease, 0, false);
        return;
    }
    DeliverKey(scancode, vk, scan, action, 0, false);
}

void SendChar(uint32_t codepoint, bool plain) {
    const UINT msg = plain ? WM_CHAR : WM_SYSCHAR;
    if (codepoint > 0xFFFF) {
        const uint32_t v = codepoint - 0x10000;
        Deliver(msg, static_cast<WPARAM>(0xD800 + (v >> 10)), g_state.lastKeyLParam);
        Deliver(msg, static_cast<WPARAM>(0xDC00 + (v & 0x3FF)), g_state.lastKeyLParam);
    } else {
        Deliver(msg, static_cast<WPARAM>(codepoint), g_state.lastKeyLParam);
    }
}

void CharModsCallback(void* window, unsigned int codepoint, int mods) {
    std::lock_guard<std::recursive_mutex> lock(g_stateMutex);
    if (window != g_state.window) return;
    // GLFW calls the plain character callback unless Command is held.
    SendChar(codepoint, (mods & kModSuper) == 0);
}

// --- mouse ------------------------------------------------------------------------------------------

POINT RealClientPixels(double x, double y) {
    const double s = Scale();
    return POINT{ static_cast<LONG>(std::lround(x * s)), static_cast<LONG>(std::lround(y * s)) };
}

POINT CurrentClientPixels() {
    POINT p{};
    GetCursorPos(&p);
    ScreenToClient(g_state.hwnd, &p);
    return p;
}

void MouseButtonCallback(void* window, int button, int action, int mods) {
    (void)mods;
    std::lock_guard<std::recursive_mutex> lock(g_stateMutex);
    if (window != g_state.window) return;
    static const unsigned kVk[] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
    if (button < 0 || button > 4) {
        if (g_state.game.mouseButton && button <= kMouseButtonLast) {
            g_state.gameButtons[button] = action == kPress;
            g_state.game.mouseButton(window, button, action, GameMods());
        }
        return;
    }
    const bool down = action == kPress;
    MacWin32::SetKeyDown(kVk[button], down);
    const POINT p = CurrentClientPixels();
    const LPARAM lParam = MAKELPARAM(static_cast<WORD>(p.x), static_cast<WORD>(p.y));
    WPARAM wParam = MouseKeyFlags();
    UINT msg = 0;
    switch (button) {
    case 0: msg = down ? WM_LBUTTONDOWN : WM_LBUTTONUP; break;
    case 1: msg = down ? WM_RBUTTONDOWN : WM_RBUTTONUP; break;
    case 2: msg = down ? WM_MBUTTONDOWN : WM_MBUTTONUP; break;
    default:
        msg = down ? WM_XBUTTONDOWN : WM_XBUTTONUP;
        wParam |= static_cast<WPARAM>((button == 3 ? XBUTTON1 : XBUTTON2) << 16);
        break;
    }
    Deliver(msg, wParam, lParam);
}

void ScrollCallback(void* window, double dx, double dy) {
    std::lock_guard<std::recursive_mutex> lock(g_stateMutex);
    if (window != g_state.window) return;
    POINT screen{};
    GetCursorPos(&screen);
    const LPARAM lParam = MAKELPARAM(static_cast<WORD>(screen.x), static_cast<WORD>(screen.y));
    if (dy != 0.0) {
        g_state.scrollRemainderY += dy * WHEEL_DELTA;
        const int delta = static_cast<int>(g_state.scrollRemainderY);
        g_state.scrollRemainderY -= delta;
        if (delta != 0) Deliver(WM_MOUSEWHEEL, MAKEWPARAM(static_cast<WORD>(MouseKeyFlags()), static_cast<WORD>(static_cast<SHORT>(delta))), lParam);
    }
    if (dx != 0.0) {
        // GLFW's Win32 backend negates horizontal wheel deltas.
        g_state.scrollRemainderX += -dx * WHEEL_DELTA;
        const int delta = static_cast<int>(g_state.scrollRemainderX);
        g_state.scrollRemainderX -= delta;
        if (delta != 0) Deliver(WM_MOUSEHWHEEL, MAKEWPARAM(static_cast<WORD>(MouseKeyFlags()), static_cast<WORD>(static_cast<SHORT>(delta))), lParam);
    }
}

void CursorPosCallback(void* window, double x, double y) {
    std::lock_guard<std::recursive_mutex> lock(g_stateMutex);
    if (window != g_state.window) return;

    if (g_state.cursorMode == kCursorDisabled && !g_state.forcedVisible) {
        // Relative motion: scale by Toolscreen's sensitivity (the Windows build
        // does this to raw input) and keep the game's own virtual position.
        if (!g_state.haveRaw) {
            g_state.lastRawX = x;
            g_state.lastRawY = y;
            g_state.haveRaw = true;
            return;
        }
        const double dx = x - g_state.lastRawX;
        const double dy = y - g_state.lastRawY;
        g_state.lastRawX = x;
        g_state.lastRawY = y;
        if (GuiOpen()) return;
        float sx = 1.0f, sy = 1.0f;
        if (g_sensitivityResolver) g_sensitivityResolver(sx, sy);
        g_state.gameCursorX += dx * sx;
        g_state.gameCursorY += dy * sy;
        if (g_state.game.cursorPos) g_state.game.cursorPos(window, g_state.gameCursorX, g_state.gameCursorY);
        return;
    }

    // Absolute positions. While Toolscreen forces the cursor visible over a
    // grabbed game, GLFW still reports its virtual position, so use the real one.
    const POINT p = g_state.forcedVisible ? CurrentClientPixels() : RealClientPixels(x, y);
    g_state.haveRaw = false;
    Deliver(WM_SETCURSOR, reinterpret_cast<WPARAM>(g_state.hwnd), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
    Deliver(WM_MOUSEMOVE, MouseKeyFlags(), MAKELPARAM(static_cast<WORD>(p.x), static_cast<WORD>(p.y)));
}

void FocusCallback(void* window, int focused) {
    std::lock_guard<std::recursive_mutex> lock(g_stateMutex);
    if (window != g_state.window) return;
    MacWin32::WindowSnapshot snapshot = MacWin32::GetWindowSnapshot(g_state.hwnd);
    snapshot.key = focused != 0;
    MacWin32::UpdateWindowSnapshot(g_state.hwnd, snapshot);
    g_state.focusChanged = true;
    Deliver(WM_ACTIVATEAPP, focused ? TRUE : FALSE, 0);
    Deliver(WM_ACTIVATE, focused ? WA_ACTIVE : WA_INACTIVE, 0);
    Deliver(focused ? WM_SETFOCUS : WM_KILLFOCUS, 0, 0);
}

void CursorEnterCallback(void* window, int entered) {
    std::lock_guard<std::recursive_mutex> lock(g_stateMutex);
    if (window != g_state.window) return;
    if (!entered && MacWin32::ConsumeMouseLeaveRequest(g_state.hwnd)) Deliver(WM_MOUSELEAVE, 0, 0);
    if (g_state.game.enter) g_state.game.enter(window, entered);
}

// Real resizes from Cocoa become WM_SIZE in pixels, as on Windows.
void RealFramebufferSizeCallback(void* window, int width, int height) {
    std::lock_guard<std::recursive_mutex> lock(g_stateMutex);
    if (window != g_state.window) return;
    MacWin32::WindowSnapshot snapshot = MacWin32::GetWindowSnapshot(g_state.hwnd);
    snapshot.clientWidth = width;
    snapshot.clientHeight = height;
    MacWin32::UpdateWindowSnapshot(g_state.hwnd, snapshot);
    Deliver(WM_SIZE, SIZE_RESTORED, MAKELPARAM(static_cast<WORD>(width), static_cast<WORD>(height)));
}

void RealWindowSizeCallback(void*, int, int) {
    // Window sizes in points follow from the framebuffer size; see GlfwWndProc.
}

// Upstream calls captured size callbacks directly with pixel sizes.
void UpstreamWindowSizeTrampoline(void* window, int width, int height) {
    if (!g_state.game.windowSize || width <= 0 || height <= 0) return;
    const double s = Scale();
    g_state.game.windowSize(window, static_cast<int>(std::lround(width / s)), static_cast<int>(std::lround(height / s)));
}

void UpstreamFramebufferSizeTrampoline(void* window, int width, int height) {
    if (g_state.game.framebufferSize && width > 0 && height > 0) g_state.game.framebufferSize(window, width, height);
}

// --- GlfwWndProc: what GLFW's Win32 window procedure would do -----------------------------------

int GlfwKeyForMessage(WPARAM wParam, LPARAM lParam, int& macKeyCode) {
    unsigned scan = static_cast<unsigned>((lParam >> 16) & 0xFF);
    if (lParam & (1 << 24)) scan |= MacKeymap::kExtended;
    macKeyCode = MacKeymap::MacKeyCodeFromScanCode(scan);
    if (macKeyCode < 0 && scan == 0) macKeyCode = MacKeymap::MacKeyCodeFromVk(static_cast<unsigned>(wParam));
    if (macKeyCode >= 0) return MacKeymap::GlfwKeyFromMacKeyCode(macKeyCode);
    return MacKeymap::GlfwKeyFromScanCode(scan);
}

void DeliverGameKey(int key, int scancode, int action) {
    if (key < 0 || key > kKeyLast) return;
    g_state.gameKeys[key] = action != kRelease;
    if (g_state.game.key) g_state.game.key(g_state.window, key, scancode, action, GameMods());
}

void DeliverGameChar(uint32_t codepoint, bool plain) {
    // GLFW drops control characters.
    if (codepoint < 32 || (codepoint > 126 && codepoint < 160)) return;
    const int mods = GameMods();
    if (g_state.game.charMods) g_state.game.charMods(g_state.window, codepoint, mods);
    if (plain && g_state.game.character) g_state.game.character(g_state.window, codepoint);
}

void DeliverGameButton(int button, int action) {
    if (button < 0 || button > kMouseButtonLast) return;
    g_state.gameButtons[button] = action == kPress;
    if (g_state.game.mouseButton) g_state.game.mouseButton(g_state.window, button, action, GameMods());
}

void DeliverGameCursor(double x, double y) {
    g_state.gameCursorX = x;
    g_state.gameCursorY = y;
    if (g_state.game.cursorPos) g_state.game.cursorPos(g_state.window, x, y);
}

LRESULT CALLBACK GlfwWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (hwnd != g_state.hwnd) return 0;
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        int macKeyCode = -1;
        const int key = GlfwKeyForMessage(wParam, lParam, macKeyCode);
        const bool up = (msg == WM_KEYUP || msg == WM_SYSKEYUP);
        int action = up ? kRelease : ((lParam & (1 << 30)) ? kRepeat : kPress);
        if (!up && action == kRepeat && key >= 0 && key <= kKeyLast && !g_state.gameKeys[key]) action = kPress;
        DeliverGameKey(key, macKeyCode >= 0 ? macKeyCode : 0, action);
        return 0;
    }
    case WM_CHAR:
    case WM_SYSCHAR:
    case WM_UNICHAR: {
        if (msg == WM_UNICHAR && wParam == UNICODE_NOCHAR) return TRUE;
        const WCHAR unit = static_cast<WCHAR>(wParam);
        uint32_t codepoint = static_cast<uint32_t>(wParam);
        if (msg != WM_UNICHAR) {
            if (unit >= 0xD800 && unit <= 0xDBFF) {
                g_state.pendingHighSurrogate = unit;
                return 0;
            }
            if (unit >= 0xDC00 && unit <= 0xDFFF) {
                if (!g_state.pendingHighSurrogate) return 0;
                codepoint = 0x10000 + ((g_state.pendingHighSurrogate - 0xD800) << 10) + (unit - 0xDC00);
                g_state.pendingHighSurrogate = 0;
            }
        }
        DeliverGameChar(codepoint, msg != WM_SYSCHAR);
        return 0;
    }
    case WM_LBUTTONDOWN: DeliverGameButton(0, kPress); return 0;
    case WM_LBUTTONUP: DeliverGameButton(0, kRelease); return 0;
    case WM_RBUTTONDOWN: DeliverGameButton(1, kPress); return 0;
    case WM_RBUTTONUP: DeliverGameButton(1, kRelease); return 0;
    case WM_MBUTTONDOWN: DeliverGameButton(2, kPress); return 0;
    case WM_MBUTTONUP: DeliverGameButton(2, kRelease); return 0;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        DeliverGameButton(GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? 3 : 4, msg == WM_XBUTTONDOWN ? kPress : kRelease);
        return TRUE;
    case WM_MOUSEMOVE: {
        if (g_state.cursorMode == kCursorDisabled) return 0;
        const double s = Scale();
        DeliverGameCursor(GET_X_LPARAM(lParam) / s, GET_Y_LPARAM(lParam) / s);
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (g_state.game.scroll) g_state.game.scroll(g_state.window, 0.0, static_cast<SHORT>(HIWORD(wParam)) / static_cast<double>(WHEEL_DELTA));
        return 0;
    case WM_MOUSEHWHEEL:
        if (g_state.game.scroll) g_state.game.scroll(g_state.window, -static_cast<SHORT>(HIWORD(wParam)) / static_cast<double>(WHEEL_DELTA), 0.0);
        return 0;
    case WM_SETFOCUS:
        if (g_state.game.focus) g_state.game.focus(g_state.window, 1);
        return 0;
    case WM_KILLFOCUS:
        if (g_state.game.focus) g_state.game.focus(g_state.window, 0);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            if (g_state.cursorMode == kCursorNormal) {
                if (g_state.gameCursor && g_real.setCursor) {
                    g_real.setCursor(g_state.window, g_state.gameCursor);
                    SetCursor(MacGdi::ArrowCursor());
                } else {
                    SetCursor(MacGdi::ArrowCursor());
                }
            } else {
                SetCursor(nullptr);
            }
            return TRUE;
        }
        return FALSE;
    case WM_SIZE: {
        const int width = LOWORD(lParam);
        const int height = HIWORD(lParam);
        if (width <= 0 || height <= 0) return 0;
        const double s = Scale();
        if (g_state.game.windowSize) {
            g_state.game.windowSize(g_state.window, static_cast<int>(std::lround(width / s)), static_cast<int>(std::lround(height / s)));
        }
        if (g_state.game.framebufferSize) g_state.game.framebufferSize(g_state.window, width, height);
        return 0;
    }
    default: return 0;
    }
}

// --- injected input (SendInput) ---------------------------------------------------------------------

void InjectInput(const INPUT& in) {
    std::lock_guard<std::recursive_mutex> lock(g_stateMutex);
    if (!g_state.hwnd) return;
    if (in.type == INPUT_KEYBOARD) {
        const KEYBDINPUT& k = in.ki;
        const bool up = (k.dwFlags & KEYEVENTF_KEYUP) != 0;
        const LPARAM extra = static_cast<LPARAM>(k.dwExtraInfo);
        if (k.dwFlags & KEYEVENTF_UNICODE) {
            if (!up) Deliver(WM_CHAR, k.wScan, 1, extra);
            return;
        }
        unsigned scan = 0;
        unsigned vk = k.wVk;
        if (k.dwFlags & KEYEVENTF_SCANCODE) {
            scan = k.wScan & 0xFF;
            if ((k.dwFlags & KEYEVENTF_EXTENDEDKEY) || (k.wScan & 0xFF00) == 0xE000) scan |= MacKeymap::kExtended;
            vk = MacKeymap::VkFromScanCode(scan, true);
        } else {
            scan = MacKeymap::ScanCodeFromVk(vk);
            if (k.dwFlags & KEYEVENTF_EXTENDEDKEY) scan |= MacKeymap::kExtended;
        }
        if (vk == VK_SHIFT) vk = (scan == 0x36) ? VK_RSHIFT : VK_LSHIFT;
        if (vk == VK_CONTROL) vk = (scan & MacKeymap::kExtended) ? VK_RCONTROL : VK_LCONTROL;
        if (vk == VK_MENU) vk = (scan & MacKeymap::kExtended) ? VK_RMENU : VK_LMENU;
        const int mac = MacKeymap::MacKeyCodeFromScanCode(scan);
        const int action = up ? kRelease : (MacWin32::IsKeyDown(vk) ? kRepeat : kPress);
        DeliverKey(mac, vk, scan, action, extra, true);
        return;
    }
    if (in.type == INPUT_MOUSE) {
        const MOUSEINPUT& m = in.mi;
        const LPARAM extra = static_cast<LPARAM>(m.dwExtraInfo);
        const POINT p = CurrentClientPixels();
        const LPARAM pos = MAKELPARAM(static_cast<WORD>(p.x), static_cast<WORD>(p.y));
        struct ButtonFlag {
            DWORD flag;
            unsigned vk;
            UINT msg;
            bool down;
        };
        static const ButtonFlag kButtons[] = {
            { MOUSEEVENTF_LEFTDOWN, VK_LBUTTON, WM_LBUTTONDOWN, true },   { MOUSEEVENTF_LEFTUP, VK_LBUTTON, WM_LBUTTONUP, false },
            { MOUSEEVENTF_RIGHTDOWN, VK_RBUTTON, WM_RBUTTONDOWN, true },  { MOUSEEVENTF_RIGHTUP, VK_RBUTTON, WM_RBUTTONUP, false },
            { MOUSEEVENTF_MIDDLEDOWN, VK_MBUTTON, WM_MBUTTONDOWN, true }, { MOUSEEVENTF_MIDDLEUP, VK_MBUTTON, WM_MBUTTONUP, false },
        };
        for (const ButtonFlag& b : kButtons) {
            if (m.dwFlags & b.flag) {
                MacWin32::SetKeyDown(b.vk, b.down);
                Deliver(b.msg, MouseKeyFlags(), pos, extra);
            }
        }
        if (m.dwFlags & (MOUSEEVENTF_XDOWN | MOUSEEVENTF_XUP)) {
            const bool down = (m.dwFlags & MOUSEEVENTF_XDOWN) != 0;
            const WORD which = (m.mouseData & XBUTTON2) ? XBUTTON2 : XBUTTON1;
            MacWin32::SetKeyDown(which == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2, down);
            Deliver(down ? WM_XBUTTONDOWN : WM_XBUTTONUP, MouseKeyFlags() | (static_cast<WPARAM>(which) << 16), pos, extra);
        }
        if (m.dwFlags & MOUSEEVENTF_WHEEL) {
            Deliver(WM_MOUSEWHEEL, MAKEWPARAM(static_cast<WORD>(MouseKeyFlags()), static_cast<WORD>(m.mouseData)), pos, extra);
        }
        if (m.dwFlags & MOUSEEVENTF_HWHEEL) {
            Deliver(WM_MOUSEHWHEEL, MAKEWPARAM(static_cast<WORD>(MouseKeyFlags()), static_cast<WORD>(m.mouseData)), pos, extra);
        }
    }
}

// --- cursor visibility ----------------------------------------------------------------------------

void OnWin32CursorVisibility(bool visible) {
    std::lock_guard<std::recursive_mutex> lock(g_stateMutex);
    if (!g_state.window) return;
    if (visible && GameHidesCursor() && !g_state.forcedVisible) {
        // Toolscreen's GUI over a grabbed cursor: release it like ShowCursor
        // does on Windows, without telling the game.
        g_state.forcedVisible = true;
        MacOs::SetCursorAssociated(true);
        MacOs::UnhideCursor();
    } else if (!visible && g_state.forcedVisible) {
        g_state.forcedVisible = false;
        if (GameHidesCursor()) {
            MacOs::HideCursor();
            if (g_state.cursorMode == kCursorDisabled) MacOs::SetCursorAssociated(false);
        }
        g_state.haveRaw = false;
    }
}

void WarpFromToolscreen(int x, int y) {
    MacOs::WarpCursor(x, y);
    // CGWarpMouseCursorPosition suppresses events briefly unless re-associated.
    if (g_state.cursorMode != kCursorDisabled || g_state.forcedVisible) MacOs::SetCursorAssociated(true);
}

// GLFW's own warps (CGWarpMouseCursorPosition / CGDisplayMoveCursorToPoint from
// libglfw) go through Toolscreen's SetCursorPos hook like GLFW-Win32's do.
struct CGPointArg {
    double x;
    double y;
};

int GlfwWarpCursor(CGPointArg point) {
    const double s = Scale();
    const int x = static_cast<int>(std::lround(point.x * s));
    const int y = static_cast<int>(std::lround(point.y * s));
    if (g_cursorPosHook && t_bridgeWarpDepth == 0) {
        ++t_bridgeWarpDepth;
        g_cursorPosHook(x, y);
        --t_bridgeWarpDepth;
        return 0;
    }
    if (g_real.warpCursor) g_real.warpCursor(point.x, point.y);
    return 0;
}

int GlfwMoveCursorToPoint(uint32_t display, CGPointArg point) {
    // Display-relative point: convert to global coordinates via the display's frame.
    for (const MacOs::Display& d : MacOs::Displays()) {
        if (d.id == display) {
            const double s = MacWin32::GetWindowSnapshot(g_state.hwnd).scale;
            CGPointArg global{ d.frame.left / s + point.x, d.frame.top / s + point.y };
            return GlfwWarpCursor(global);
        }
    }
    return g_real.moveCursorToPoint ? g_real.moveCursorToPoint(display, point.x, point.y) : 0;
}

// --- window lifecycle -------------------------------------------------------------------------------

void RefreshSnapshot(bool includeTitle) {
    if (!g_state.nsWindow || !g_state.hwnd) return;
    MacWin32::WindowSnapshot s = MacWin32::GetWindowSnapshot(g_state.hwnd);
    s.scale = MacOs::WindowBackingScale(g_state.nsWindow);
    MacOs::SetCoordinateScale(s.scale);
    MacOs::PixelRect content, frame;
    if (MacOs::WindowContentRect(g_state.nsWindow, content)) s.clientScreen = RECT{ content.left, content.top, content.right, content.bottom };
    if (MacOs::WindowFrameRect(g_state.nsWindow, frame)) s.frameScreen = RECT{ frame.left, frame.top, frame.right, frame.bottom };
    s.key = MacOs::WindowIsKey(g_state.nsWindow);
    s.visible = MacOs::WindowIsVisible(g_state.nsWindow);
    s.minimized = MacOs::WindowIsMiniaturized(g_state.nsWindow);
    s.zoomed = MacOs::WindowIsZoomed(g_state.nsWindow);
    s.fullscreen = MacOs::WindowIsFullscreen(g_state.nsWindow);
    s.borderless = MacOs::WindowIsBorderless(g_state.nsWindow);
    int w = 0, h = 0;
    if (g_real.getFramebufferSize) {
        g_real.getFramebufferSize(g_state.window, &w, &h);
        if (w > 0 && h > 0) {
            s.clientWidth = w;
            s.clientHeight = h;
        }
    }
    if (includeTitle) s.title = MacOs::WindowTitle(g_state.nsWindow);
    MacWin32::UpdateWindowSnapshot(g_state.hwnd, s);
}

template <typename Setter, typename Fun> void InstallRealCallback(Setter setter, Fun fun) {
    if (setter) setter(g_state.window, fun);
}

void OnWindowCreated(void* window) {
    std::lock_guard<std::recursive_mutex> lock(g_stateMutex);
    ResolveLazy(g_real.getCocoaWindow, "glfwGetCocoaWindow");
    ResolveLazy(g_real.getFramebufferSize, "glfwGetFramebufferSize");
    ResolveLazy(g_real.getInputMode, "glfwGetInputMode");
    ResolveLazy(g_real.setKeyCallback, "glfwSetKeyCallback");
    ResolveLazy(g_real.setCharCallback, "glfwSetCharCallback");
    ResolveLazy(g_real.setCharModsCallback, "glfwSetCharModsCallback");
    ResolveLazy(g_real.setMouseButtonCallback, "glfwSetMouseButtonCallback");
    ResolveLazy(g_real.setCursorPosCallback, "glfwSetCursorPosCallback");
    ResolveLazy(g_real.setScrollCallback, "glfwSetScrollCallback");
    ResolveLazy(g_real.setWindowFocusCallback, "glfwSetWindowFocusCallback");
    ResolveLazy(g_real.setCursorEnterCallback, "glfwSetCursorEnterCallback");
    ResolveLazy(g_real.setWindowSizeCallback, "glfwSetWindowSizeCallback");
    ResolveLazy(g_real.setFramebufferSizeCallback, "glfwSetFramebufferSizeCallback");
    ResolveLazy(g_real.getCursorPos, "glfwGetCursorPos");
    ResolveLazy(g_real.setCursorPos, "glfwSetCursorPos");
    ResolveLazy(g_real.setInputMode, "glfwSetInputMode");
    ResolveLazy(g_real.setCursor, "glfwSetCursor");

    g_state = BridgeState{};
    g_state.window = window;
    g_state.nsWindow = g_real.getCocoaWindow ? g_real.getCocoaWindow(window) : nullptr;
    g_state.hwnd = MacWin32::RegisterGameWindow(window, g_state.nsWindow, &GlfwWndProc);
    RefreshSnapshot(true);
    MacOs::RefreshKeyboardLayout();

    InstallRealCallback(g_real.setKeyCallback, &KeyCallback);
    InstallRealCallback(g_real.setCharModsCallback, &CharModsCallback);
    InstallRealCallback(g_real.setMouseButtonCallback, &MouseButtonCallback);
    InstallRealCallback(g_real.setCursorPosCallback, &CursorPosCallback);
    InstallRealCallback(g_real.setScrollCallback, &ScrollCallback);
    InstallRealCallback(g_real.setWindowFocusCallback, &FocusCallback);
    InstallRealCallback(g_real.setCursorEnterCallback, &CursorEnterCallback);
    InstallRealCallback(g_real.setWindowSizeCallback, &RealWindowSizeCallback);
    InstallRealCallback(g_real.setFramebufferSizeCallback, &RealFramebufferSizeCallback);
    if (g_real.getCursorPos) {
        double x = 0, y = 0;
        g_real.getCursorPos(window, &x, &y);
        g_state.gameCursorX = x;
        g_state.gameCursorY = y;
    }
}

// --- detours for the game's GLFW calls ---------------------------------------------------------------

template <typename Fun> Fun SwapCallback(Fun& slot, void* window, Fun callback) {
    std::lock_guard<std::recursive_mutex> lock(g_stateMutex);
    if (window != g_state.window) return nullptr;
    Fun previous = slot;
    slot = callback;
    return previous;
}

KeyFun HookSetKeyCallback(void* window, KeyFun cb) {
    if (window != g_state.window && g_real.setKeyCallback) return g_real.setKeyCallback(window, cb);
    return SwapCallback(g_state.game.key, window, cb);
}
CharFun HookSetCharCallback(void* window, CharFun cb) {
    if (window != g_state.window && g_real.setCharCallback) return g_real.setCharCallback(window, cb);
    return SwapCallback(g_state.game.character, window, cb);
}
CharModsFun HookSetCharModsCallback(void* window, CharModsFun cb) {
    if (window != g_state.window && g_real.setCharModsCallback) return g_real.setCharModsCallback(window, cb);
    return SwapCallback(g_state.game.charMods, window, cb);
}
MouseButtonFun HookSetMouseButtonCallback(void* window, MouseButtonFun cb) {
    if (window != g_state.window && g_real.setMouseButtonCallback) return g_real.setMouseButtonCallback(window, cb);
    return SwapCallback(g_state.game.mouseButton, window, cb);
}
CursorPosFun HookSetCursorPosCallback(void* window, CursorPosFun cb) {
    if (window != g_state.window && g_real.setCursorPosCallback) return g_real.setCursorPosCallback(window, cb);
    return SwapCallback(g_state.game.cursorPos, window, cb);
}
ScrollFun HookSetScrollCallback(void* window, ScrollFun cb) {
    if (window != g_state.window && g_real.setScrollCallback) return g_real.setScrollCallback(window, cb);
    return SwapCallback(g_state.game.scroll, window, cb);
}
FocusFun HookSetWindowFocusCallback(void* window, FocusFun cb) {
    if (window != g_state.window && g_real.setWindowFocusCallback) return g_real.setWindowFocusCallback(window, cb);
    return SwapCallback(g_state.game.focus, window, cb);
}
EnterFun HookSetCursorEnterCallback(void* window, EnterFun cb) {
    if (window != g_state.window && g_real.setCursorEnterCallback) return g_real.setCursorEnterCallback(window, cb);
    return SwapCallback(g_state.game.enter, window, cb);
}

SizeFun HookSetWindowSizeCallback(void* window, SizeFun cb) {
    if (window != g_state.window) return g_real.setWindowSizeCallback ? g_real.setWindowSizeCallback(window, cb) : nullptr;
    const SizeFun previous = SwapCallback(g_state.game.windowSize, window, cb);
    if (g_upstreamSetWindowSize) g_upstreamSetWindowSize(window, &UpstreamWindowSizeTrampoline);
    return previous;
}

SizeFun HookSetFramebufferSizeCallback(void* window, SizeFun cb) {
    if (window != g_state.window) return g_real.setFramebufferSizeCallback ? g_real.setFramebufferSizeCallback(window, cb) : nullptr;
    const SizeFun previous = SwapCallback(g_state.game.framebufferSize, window, cb);
    if (g_upstreamSetFramebufferSize) g_upstreamSetFramebufferSize(window, &UpstreamFramebufferSizeTrampoline);
    return previous;
}

int HookGetKey(void* window, int key) {
    if (window == g_state.window && key >= 0 && key <= kKeyLast) return g_state.gameKeys[key] ? kPress : kRelease;
    return g_real.getKey ? g_real.getKey(window, key) : kRelease;
}

int HookGetMouseButton(void* window, int button) {
    if (window == g_state.window && button >= 0 && button <= kMouseButtonLast) return g_state.gameButtons[button] ? kPress : kRelease;
    return g_real.getMouseButton ? g_real.getMouseButton(window, button) : kRelease;
}

void HookGetCursorPos(void* window, double* x, double* y) {
    if (window == g_state.window) {
        if (x) *x = g_state.gameCursorX;
        if (y) *y = g_state.gameCursorY;
        return;
    }
    if (g_real.getCursorPos) g_real.getCursorPos(window, x, y);
}

void HookSetCursorPos(void* window, double x, double y) {
    if (window != g_state.window || !g_real.setCursorPos) {
        if (g_real.setCursorPos) g_real.setCursorPos(window, x, y);
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(g_stateMutex);
    g_state.gameCursorX = x;
    g_state.gameCursorY = y;
    if (g_state.cursorMode == kCursorDisabled) {
        g_real.setCursorPos(window, x, y);
        g_state.lastRawX = x;
        g_state.lastRawY = y;
        g_state.haveRaw = true;
        return;
    }
    // The game positions the cursor in its own (mode-sized) client space.
    const double s = Scale();
    int px = static_cast<int>(std::lround(x * s));
    int py = static_cast<int>(std::lround(y * s));
    MapGameClientPointToWindowClient(g_state.hwnd, px, py);
    g_real.setCursorPos(window, px / s, py / s);
}

void HookPollEvents() {
    if (g_real.pollEvents) g_real.pollEvents();
    MacWin32::PumpMessages();
}

void HookWaitEvents() {
    if (g_real.waitEvents) g_real.waitEvents();
    MacWin32::PumpMessages();
}

void HookWaitEventsTimeout(double timeout) {
    if (g_real.waitEventsTimeout) g_real.waitEventsTimeout(timeout);
    MacWin32::PumpMessages();
}

} // namespace

namespace MacGlfwBridge {

void RegisterHooks() {
    MacSymbolHooks::RegisterDlsymHook("glfwSetKeyCallback", reinterpret_cast<void*>(&HookSetKeyCallback),
                                      reinterpret_cast<void**>(&g_real.setKeyCallback));
    MacSymbolHooks::RegisterDlsymHook("glfwSetCharCallback", reinterpret_cast<void*>(&HookSetCharCallback),
                                      reinterpret_cast<void**>(&g_real.setCharCallback));
    MacSymbolHooks::RegisterDlsymHook("glfwSetCharModsCallback", reinterpret_cast<void*>(&HookSetCharModsCallback),
                                      reinterpret_cast<void**>(&g_real.setCharModsCallback));
    MacSymbolHooks::RegisterDlsymHook("glfwSetMouseButtonCallback", reinterpret_cast<void*>(&HookSetMouseButtonCallback),
                                      reinterpret_cast<void**>(&g_real.setMouseButtonCallback));
    MacSymbolHooks::RegisterDlsymHook("glfwSetCursorPosCallback", reinterpret_cast<void*>(&HookSetCursorPosCallback),
                                      reinterpret_cast<void**>(&g_real.setCursorPosCallback));
    MacSymbolHooks::RegisterDlsymHook("glfwSetScrollCallback", reinterpret_cast<void*>(&HookSetScrollCallback),
                                      reinterpret_cast<void**>(&g_real.setScrollCallback));
    MacSymbolHooks::RegisterDlsymHook("glfwSetWindowFocusCallback", reinterpret_cast<void*>(&HookSetWindowFocusCallback),
                                      reinterpret_cast<void**>(&g_real.setWindowFocusCallback));
    MacSymbolHooks::RegisterDlsymHook("glfwSetCursorEnterCallback", reinterpret_cast<void*>(&HookSetCursorEnterCallback),
                                      reinterpret_cast<void**>(&g_real.setCursorEnterCallback));
    MacSymbolHooks::RegisterDlsymHook("glfwSetWindowSizeCallback", reinterpret_cast<void*>(&HookSetWindowSizeCallback),
                                      reinterpret_cast<void**>(&g_real.setWindowSizeCallback));
    MacSymbolHooks::RegisterDlsymHook("glfwSetFramebufferSizeCallback", reinterpret_cast<void*>(&HookSetFramebufferSizeCallback),
                                      reinterpret_cast<void**>(&g_real.setFramebufferSizeCallback));
    MacSymbolHooks::RegisterDlsymHook("glfwGetKey", reinterpret_cast<void*>(&HookGetKey), reinterpret_cast<void**>(&g_real.getKey));
    MacSymbolHooks::RegisterDlsymHook("glfwGetMouseButton", reinterpret_cast<void*>(&HookGetMouseButton),
                                      reinterpret_cast<void**>(&g_real.getMouseButton));
    MacSymbolHooks::RegisterDlsymHook("glfwGetCursorPos", reinterpret_cast<void*>(&HookGetCursorPos),
                                      reinterpret_cast<void**>(&g_real.getCursorPos));
    MacSymbolHooks::RegisterDlsymHook("glfwSetCursorPos", reinterpret_cast<void*>(&HookSetCursorPos),
                                      reinterpret_cast<void**>(&g_real.setCursorPos));
    MacSymbolHooks::RegisterDlsymHook("glfwPollEvents", reinterpret_cast<void*>(&HookPollEvents),
                                      reinterpret_cast<void**>(&g_real.pollEvents));
    MacSymbolHooks::RegisterDlsymHook("glfwWaitEvents", reinterpret_cast<void*>(&HookWaitEvents),
                                      reinterpret_cast<void**>(&g_real.waitEvents));
    MacSymbolHooks::RegisterDlsymHook("glfwWaitEventsTimeout", reinterpret_cast<void*>(&HookWaitEventsTimeout),
                                      reinterpret_cast<void**>(&g_real.waitEventsTimeout));

    MacSymbolHooks::RegisterImportHook("libglfw", "CGWarpMouseCursorPosition", reinterpret_cast<void*>(&GlfwWarpCursor),
                                       reinterpret_cast<void**>(&g_real.warpCursor));
    MacSymbolHooks::RegisterImportHook("libglfw", "CGDisplayMoveCursorToPoint", reinterpret_cast<void*>(&GlfwMoveCursorToPoint),
                                       reinterpret_cast<void**>(&g_real.moveCursorToPoint));

    MacWin32::SetInputInjector(&InjectInput);
    MacWin32::CursorCallbacks callbacks;
    callbacks.visibilityChanged = &OnWin32CursorVisibility;
    callbacks.gameHidesCursor = [] { return GameHidesCursor() && !g_state.forcedVisible; };
    callbacks.warp = &WarpFromToolscreen;
    MacWin32::SetCursorCallbacks(std::move(callbacks));
}

void* CreateWindowThroughBridge(int width, int height, const char* title, void* monitor, void* share) {
    if (!g_real.createWindow) return nullptr;
    void* window = g_real.createWindow(width, height, title, monitor, share);
    if (window) OnWindowCreated(window);
    return window;
}

void DestroyWindowThroughBridge(void* window) {
    {
        std::lock_guard<std::recursive_mutex> lock(g_stateMutex);
        if (window == g_state.window) {
            MacWin32::UnregisterGameWindow(g_state.hwnd);
            g_state = BridgeState{};
        }
    }
    if (g_real.destroyWindow) g_real.destroyWindow(window);
}

void SetInputModeThroughBridge(void* window, int mode, int value) {
    if (!g_real.setInputMode) return;
    std::lock_guard<std::recursive_mutex> lock(g_stateMutex);
    if (window == g_state.window) {
        if (mode == kLockKeyMods) g_state.lockKeyMods = value != 0;
        if (mode == kCursor) {
            if (g_state.forcedVisible) {
                // Toolscreen released a grabbed cursor; undo that before GLFW changes mode.
                g_state.forcedVisible = false;
                MacOs::HideCursor();
            }
            g_state.cursorMode = value;
            g_state.haveRaw = false;
        }
    }
    g_real.setInputMode(window, mode, value);
    if (window == g_state.window && mode == kCursor && value == kCursorDisabled && g_real.getCursorPos) {
        double x = 0, y = 0;
        g_real.getCursorPos(window, &x, &y);
        g_state.lastRawX = x;
        g_state.lastRawY = y;
        g_state.haveRaw = true;
    }
    if (window == g_state.window && mode == kCursor) {
        // GLFW's Win32 backend updates the cursor image right away on a mode
        // change; do the same so cursor visibility is current immediately.
        Deliver(WM_SETCURSOR, reinterpret_cast<WPARAM>(g_state.hwnd), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
    }
}

void SetCursorThroughBridge(void* window, void* cursor) {
    {
        std::lock_guard<std::recursive_mutex> lock(g_stateMutex);
        if (window == g_state.window) g_state.gameCursor = cursor;
    }
    if (g_real.setCursor) g_real.setCursor(window, cursor);
}

WindowSizeFun SetWindowSizeCallbackThroughBridge(void* window, WindowSizeFun) {
    // Upstream registers the bridge's trampoline; Cocoa resizes stay with the bridge.
    if (g_real.setWindowSizeCallback) g_real.setWindowSizeCallback(window, &RealWindowSizeCallback);
    return nullptr;
}

WindowSizeFun SetFramebufferSizeCallbackThroughBridge(void* window, WindowSizeFun) {
    if (g_real.setFramebufferSizeCallback) g_real.setFramebufferSizeCallback(window, &RealFramebufferSizeCallback);
    return nullptr;
}

void SetUpstreamSizeCallbackSetters(SetSizeCallbackFun windowSize, SetSizeCallbackFun framebufferSize) {
    g_upstreamSetWindowSize = windowSize;
    g_upstreamSetFramebufferSize = framebufferSize;
}

void** RealCreateWindowSlot() { return reinterpret_cast<void**>(&g_real.createWindow); }
void** RealDestroyWindowSlot() { return reinterpret_cast<void**>(&g_real.destroyWindow); }
void** RealSetInputModeSlot() { return reinterpret_cast<void**>(&g_real.setInputMode); }
void** RealSetCursorSlot() { return reinterpret_cast<void**>(&g_real.setCursor); }
void** RealSetWindowSizeCallbackSlot() { return reinterpret_cast<void**>(&g_real.setWindowSizeCallback); }
void** RealSetFramebufferSizeCallbackSlot() { return reinterpret_cast<void**>(&g_real.setFramebufferSizeCallback); }
void** RealSwapBuffersSlot() { return reinterpret_cast<void**>(&g_real.swapBuffers); }
void** RealWindowHintSlot() { return reinterpret_cast<void**>(&g_real.windowHint); }

void BeforeSwap(void* window) {
    std::lock_guard<std::recursive_mutex> lock(g_stateMutex);
    if (window != g_state.window) return;
    const bool includeTitle = g_state.titleCountdown-- <= 0;
    if (includeTitle) g_state.titleCountdown = 60;
    RefreshSnapshot(includeTitle);

    if (g_state.forcedVisible && g_state.focusChanged) {
        // GLFW re-grabs the cursor when the window regains focus.
        MacOs::SetCursorAssociated(true);
    }
    g_state.focusChanged = false;

    // ClipCursor: keep a visible cursor inside the clip rectangle.
    RECT clip{};
    const MacWin32::WindowSnapshot snapshot = MacWin32::GetWindowSnapshot(g_state.hwnd);
    if (snapshot.key && (g_state.cursorMode == kCursorNormal || g_state.forcedVisible) && MacWin32::GetCursorClip(clip)) {
        POINT p{};
        if (GetCursorPos(&p)) {
            const LONG cx = std::clamp<LONG>(p.x, clip.left, std::max<LONG>(clip.left, clip.right - 1));
            const LONG cy = std::clamp<LONG>(p.y, clip.top, std::max<LONG>(clip.top, clip.bottom - 1));
            if (cx != p.x || cy != p.y) WarpFromToolscreen(cx, cy);
        }
    }

    MacWin32::PumpMessages();
}

bool RealFramebufferSize(void* window, int& width, int& height) {
    width = height = 0;
    ResolveLazy(g_real.getFramebufferSize, "glfwGetFramebufferSize");
    if (!g_real.getFramebufferSize) return false;
    g_real.getFramebufferSize(window, &width, &height);
    return width > 0 && height > 0;
}

void RealSwapBuffers(void* window) {
    if (g_real.swapBuffers) g_real.swapBuffers(window);
}

void SetSensitivityResolver(void (*resolver)(float&, float&)) { g_sensitivityResolver = resolver; }
void SetCursorPosHook(BOOL (*hook)(int, int)) { g_cursorPosHook = hook; }

BOOL WarpCursorScreen(int x, int y) {
    const double s = MacWin32::GetWindowSnapshot(g_state.hwnd).scale;
    const double scale = s > 0.0 ? s : 1.0;
    if (g_real.warpCursor) g_real.warpCursor(x / scale, y / scale);
    else MacOs::WarpCursor(x, y);
    if (g_state.cursorMode != kCursorDisabled || g_state.forcedVisible) MacOs::SetCursorAssociated(true);
    return TRUE;
}

bool IsGuiOpen() { return GuiOpen(); }
void SetGuiOpenQuery(bool (*query)()) { g_guiOpenQuery = query; }

} // namespace MacGlfwBridge
