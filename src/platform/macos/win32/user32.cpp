// user32 subset: windows, messages, keyboard/mouse state, cursor, monitors,
// clipboard, dialogs and system parameters.

#include "platform/macos/win32/user32_internal.h"

#include "platform/macos/os/mac_os.h"
#include "platform/macos/win32/gdi_internal.h"
#include "platform/macos/win32/keymap.h"
#include "platform/macos/win32/win32_internal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" void* CGLGetCurrentContext(void);

namespace {

// --- windows ------------------------------------------------------------------------------

struct EmulatedWindow {
    HWND hwnd = nullptr;
    void* nsWindow = nullptr;
    WNDPROC proc = nullptr;
    WNDPROC defaultProc = nullptr;
    LONG_PTR userData = 0;
    LONG_PTR style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    LONG_PTR exStyle = 0;
    std::map<std::wstring, HANDLE> props;
    MacWin32::WindowSnapshot snapshot;
};

std::mutex g_windowMutex;
std::unique_ptr<EmulatedWindow> g_window;
std::atomic<DWORD> g_windowThreadId{ 0 };

// HWNDs for other applications' windows carry the CGWindowID with this tag.
constexpr uintptr_t kForeignTag = 0x7E00000000000000ull;
const HWND kDesktopWindow = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x7D00000000000001ull));

bool IsForeign(HWND hwnd) { return (reinterpret_cast<uintptr_t>(hwnd) & 0xFF00000000000000ull) == kForeignTag; }
uint32_t ForeignId(HWND hwnd) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(hwnd) & 0xFFFFFFFFu); }
HWND ForeignHwnd(uint32_t id) { return reinterpret_cast<HWND>(kForeignTag | id); }

EmulatedWindow* FindWindowLocked(HWND hwnd) {
    if (g_window && g_window->hwnd == hwnd && hwnd) return g_window.get();
    return nullptr;
}

// --- message context -------------------------------------------------------------------------

thread_local int t_dispatchDepth = 0;
thread_local LPARAM t_messageExtraInfo = 0;
thread_local LONG t_messageTime = 0;

struct PostedMessage {
    HWND hwnd;
    UINT msg;
    WPARAM wParam;
    LPARAM lParam;
};

std::mutex g_postMutex;
std::deque<PostedMessage> g_posted;

struct Timer {
    HWND hwnd;
    UINT_PTR id;
    UINT elapse;
    uint64_t nextMs;
};
std::mutex g_timerMutex;
std::vector<Timer> g_timers;

struct ThreadQueue {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<MSG> messages;
};
std::mutex g_threadQueuesMutex;
std::unordered_map<DWORD, std::shared_ptr<ThreadQueue>> g_threadQueues;

std::shared_ptr<ThreadQueue> QueueFor(DWORD threadId, bool create) {
    std::lock_guard<std::mutex> lock(g_threadQueuesMutex);
    auto it = g_threadQueues.find(threadId);
    if (it != g_threadQueues.end()) return it->second;
    if (!create) return nullptr;
    auto q = std::make_shared<ThreadQueue>();
    g_threadQueues[threadId] = q;
    return q;
}

std::mutex g_injectMutex;
std::deque<INPUT> g_injected;
std::function<void(const INPUT&)> g_injector;

std::mutex g_registeredMessagesMutex;
std::map<std::string, UINT> g_registeredMessages;

// --- keyboard state ----------------------------------------------------------------------------

std::atomic<uint8_t> g_keyDown[256];
std::atomic<uint8_t> g_keyToggled[256];

struct LowLevelHook {
    HHOOK id;
    int type;
    HOOKPROC proc;
};
std::mutex g_hookMutex;
std::vector<LowLevelHook> g_hooks;
std::atomic<uintptr_t> g_nextHookId{ 1 };

// --- cursor --------------------------------------------------------------------------------------

std::mutex g_cursorMutex;
MacWin32::CursorCallbacks g_cursorCallbacks;
std::atomic<int> g_showCount{ 0 };
std::atomic<HCURSOR> g_currentCursor{ nullptr };
std::atomic<bool> g_cursorHiddenBySetCursor{ false };
std::atomic<bool> g_lastReportedVisible{ true };
bool g_haveClip = false;
RECT g_clip{};
std::atomic<HWND> g_capture{ nullptr };
std::atomic<bool> g_trackLeave{ false };

// --- clipboard -------------------------------------------------------------------------------------

std::mutex g_clipboardMutex;
std::string g_pendingClipboard;
bool g_pendingClipboardSet = false;

// --- system parameters -------------------------------------------------------------------------------

std::atomic<int> g_mouseSpeed{ 10 };
FILTERKEYS g_filterKeys{ sizeof(FILTERKEYS), 0, 0, 0, 0, 0 };

uint64_t NowMs() { return GetTickCount64(); }

LRESULT CallProc(WNDPROC proc, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return proc ? proc(hwnd, msg, wParam, lParam) : 0;
}

void NotifyCursorVisibility() {
    const bool visible = MacWin32::IsWin32CursorVisible();
    if (g_lastReportedVisible.exchange(visible) == visible) return;
    std::function<void(bool)> cb;
    {
        std::lock_guard<std::mutex> lock(g_cursorMutex);
        cb = g_cursorCallbacks.visibilityChanged;
    }
    if (cb) cb(visible);
}

std::vector<MacOs::Display> CachedDisplays() {
    static std::mutex mutex;
    static std::vector<MacOs::Display> cache;
    static uint64_t cacheMs = 0;
    std::lock_guard<std::mutex> lock(mutex);
    const uint64_t now = NowMs();
    if (cache.empty() || now - cacheMs > 1000) {
        cache = MacOs::Displays();
        cacheMs = now;
    }
    return cache;
}

HMONITOR MonitorHandle(size_t index) { return reinterpret_cast<HMONITOR>(index + 1); }

size_t MonitorIndexForRect(const RECT& r) {
    const auto displays = CachedDisplays();
    size_t best = 0;
    long bestArea = -1;
    for (size_t i = 0; i < displays.size(); ++i) {
        const auto& f = displays[i].frame;
        const long w = std::max<long>(0, std::min<long>(r.right, f.right) - std::max<long>(r.left, f.left));
        const long h = std::max<long>(0, std::min<long>(r.bottom, f.bottom) - std::max<long>(r.top, f.top));
        if (w * h > bestArea) {
            bestArea = w * h;
            best = i;
        }
    }
    return best;
}

RECT ToRect(const MacOs::PixelRect& r) { return RECT{ r.left, r.top, r.right, r.bottom }; }

bool ForeignInfo(HWND hwnd, MacOs::ForeignWindow& out) { return IsForeign(hwnd) && MacOs::WindowInfo(ForeignId(hwnd), out); }

std::string ToUtf8(LPCWSTR s) { return MacWin32::WideToUtf8(s ? s : L""); }

} // namespace

// ---------------------------------------------------------------------------------------------------
// Internal API
// ---------------------------------------------------------------------------------------------------
namespace MacWin32 {

HWND RegisterGameWindow(void* glfwWindow, void* nsWindow, WNDPROC defaultProc) {
    std::lock_guard<std::mutex> lock(g_windowMutex);
    g_window = std::make_unique<EmulatedWindow>();
    g_window->hwnd = static_cast<HWND>(glfwWindow);
    g_window->nsWindow = nsWindow;
    g_window->proc = defaultProc;
    g_window->defaultProc = defaultProc;
    g_windowThreadId.store(GetCurrentThreadId());
    return g_window->hwnd;
}

void UnregisterGameWindow(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_windowMutex);
    if (g_window && g_window->hwnd == hwnd) g_window.reset();
}

HWND GameWindow() {
    std::lock_guard<std::mutex> lock(g_windowMutex);
    return g_window ? g_window->hwnd : nullptr;
}

bool IsGameWindow(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_windowMutex);
    return FindWindowLocked(hwnd) != nullptr;
}

void* NativeWindow(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_windowMutex);
    EmulatedWindow* w = FindWindowLocked(hwnd);
    return w ? w->nsWindow : nullptr;
}

void UpdateWindowSnapshot(HWND hwnd, const WindowSnapshot& snapshot) {
    std::lock_guard<std::mutex> lock(g_windowMutex);
    if (EmulatedWindow* w = FindWindowLocked(hwnd)) {
        w->snapshot = snapshot;
        w->style = snapshot.borderless || snapshot.fullscreen ? (WS_POPUP | WS_VISIBLE) : (WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        if (snapshot.minimized) w->style |= WS_MINIMIZE;
        if (snapshot.zoomed) w->style |= WS_MAXIMIZE;
    }
}

WindowSnapshot GetWindowSnapshot(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_windowMutex);
    EmulatedWindow* w = FindWindowLocked(hwnd);
    return w ? w->snapshot : WindowSnapshot{};
}

LRESULT DispatchToWindow(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LPARAM extraInfo) {
    WNDPROC proc = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_windowMutex);
        if (EmulatedWindow* w = FindWindowLocked(hwnd)) proc = w->proc;
    }
    if (!proc) return 0;
    const LPARAM previousExtra = t_messageExtraInfo;
    const LONG previousTime = t_messageTime;
    t_messageExtraInfo = extraInfo;
    t_messageTime = static_cast<LONG>(GetTickCount());
    ++t_dispatchDepth;
    const LRESULT result = proc(hwnd, msg, wParam, lParam);
    --t_dispatchDepth;
    t_messageExtraInfo = previousExtra;
    t_messageTime = previousTime;
    return result;
}

bool InDispatch() { return t_dispatchDepth > 0; }

void PumpMessages() {
    // Posted messages are retrieved before input on Windows, so drain them
    // first and repeat while handlers post more (bounded to avoid livelock).
    for (int round = 0; round < 64; ++round) {
        bool didWork = false;

        std::deque<PostedMessage> posted;
        {
            std::lock_guard<std::mutex> lock(g_postMutex);
            posted.swap(g_posted);
        }
        for (const PostedMessage& m : posted) {
            DispatchToWindow(m.hwnd, m.msg, m.wParam, m.lParam);
            didWork = true;
        }

        std::vector<Timer> due;
        {
            std::lock_guard<std::mutex> lock(g_timerMutex);
            const uint64_t now = NowMs();
            for (Timer& t : g_timers) {
                if (now >= t.nextMs) {
                    due.push_back(t);
                    t.nextMs = now + std::max<UINT>(t.elapse, 1);
                }
            }
        }
        for (const Timer& t : due) {
            DispatchToWindow(t.hwnd, WM_TIMER, static_cast<WPARAM>(t.id), 0);
            didWork = true;
        }

        std::deque<INPUT> injected;
        std::function<void(const INPUT&)> injector;
        {
            std::lock_guard<std::mutex> lock(g_injectMutex);
            injected.swap(g_injected);
            injector = g_injector;
        }
        for (const INPUT& in : injected) {
            if (injector) injector(in);
            didWork = true;
        }

        if (!didWork) break;
    }
}

void SetKeyDown(unsigned vk, bool down) {
    if (vk == 0 || vk > 255) return;
    const uint8_t was = g_keyDown[vk].exchange(down ? 1 : 0);
    if (down && !was && (vk == VK_CAPITAL || vk == VK_NUMLOCK || vk == VK_SCROLL)) {
        g_keyToggled[vk].store(g_keyToggled[vk].load() ? 0 : 1);
    }
    auto syncGeneric = [](unsigned generic, unsigned left, unsigned right) {
        g_keyDown[generic].store((g_keyDown[left].load() || g_keyDown[right].load()) ? 1 : 0);
    };
    switch (vk) {
    case VK_LSHIFT: case VK_RSHIFT: syncGeneric(VK_SHIFT, VK_LSHIFT, VK_RSHIFT); break;
    case VK_LCONTROL: case VK_RCONTROL: syncGeneric(VK_CONTROL, VK_LCONTROL, VK_RCONTROL); break;
    case VK_LMENU: case VK_RMENU: syncGeneric(VK_MENU, VK_LMENU, VK_RMENU); break;
    case VK_SHIFT: g_keyDown[VK_LSHIFT].store(down ? 1 : 0); break;
    case VK_CONTROL: g_keyDown[VK_LCONTROL].store(down ? 1 : 0); break;
    case VK_MENU: g_keyDown[VK_LMENU].store(down ? 1 : 0); break;
    default: break;
    }
}

void ReleaseAllKeys() {
    for (auto& k : g_keyDown) k.store(0);
}

bool IsKeyDown(unsigned vk) { return vk < 256 && g_keyDown[vk].load() != 0; }

bool RunLowLevelKeyboardHooks(WPARAM msg, const KBDLLHOOKSTRUCT& info) {
    std::vector<LowLevelHook> hooks;
    {
        std::lock_guard<std::mutex> lock(g_hookMutex);
        hooks = g_hooks;
    }
    KBDLLHOOKSTRUCT copy = info;
    for (const LowLevelHook& h : hooks) {
        if (h.type != WH_KEYBOARD_LL || !h.proc) continue;
        if (h.proc(HC_ACTION, msg, reinterpret_cast<LPARAM>(&copy)) != 0) return true;
    }
    return false;
}

bool HasLowLevelKeyboardHooks() {
    std::lock_guard<std::mutex> lock(g_hookMutex);
    for (const LowLevelHook& h : g_hooks) {
        if (h.type == WH_KEYBOARD_LL) return true;
    }
    return false;
}

void SetInputInjector(std::function<void(const INPUT&)> injector) {
    std::lock_guard<std::mutex> lock(g_injectMutex);
    g_injector = std::move(injector);
}

void SetCursorCallbacks(CursorCallbacks callbacks) {
    std::lock_guard<std::mutex> lock(g_cursorMutex);
    g_cursorCallbacks = std::move(callbacks);
}

bool IsWin32CursorVisible() {
    if (g_showCount.load() < 0) return false;
    if (g_cursorHiddenBySetCursor.load()) return false;
    std::function<bool()> gameHides;
    {
        std::lock_guard<std::mutex> lock(g_cursorMutex);
        gameHides = g_cursorCallbacks.gameHidesCursor;
    }
    // ShowCursor(TRUE) beyond the default count forces the cursor visible even
    // while the game hides it (Toolscreen's GUI over a grabbed cursor).
    if (gameHides && gameHides() && g_showCount.load() <= 0) return false;
    return true;
}

HCURSOR CurrentCursor() { return g_currentCursor.load(); }

void ReapplyCursor() {
    HCURSOR cursor = g_currentCursor.load();
    if (cursor && !g_cursorHiddenBySetCursor.load()) {
        if (void* native = MacGdi::NativeCursor(cursor)) MacOs::SetCursor(native);
    }
}

bool GetCursorClip(RECT& rect) {
    std::lock_guard<std::mutex> lock(g_cursorMutex);
    if (!g_haveClip) return false;
    rect = g_clip;
    return true;
}

bool ConsumeMouseLeaveRequest(HWND) { return g_trackLeave.exchange(false); }

DWORD WindowThreadId() { return g_windowThreadId.load(); }

bool ForeignWindowId(HWND hwnd, uint32_t& windowId) {
    if (!IsForeign(hwnd)) return false;
    windowId = ForeignId(hwnd);
    return true;
}

} // namespace MacWin32

// ---------------------------------------------------------------------------------------------------
// Win32 API
// ---------------------------------------------------------------------------------------------------
extern "C" {

// --- windows ------------------------------------------------------------------------------------------

BOOL IsWindow(HWND hwnd) {
    if (!hwnd) return FALSE;
    if (MacWin32::IsGameWindow(hwnd)) return TRUE;
    MacOs::ForeignWindow info;
    return ForeignInfo(hwnd, info) ? TRUE : FALSE;
}

BOOL IsWindowVisible(HWND hwnd) {
    if (MacWin32::IsGameWindow(hwnd)) return MacWin32::GetWindowSnapshot(hwnd).visible ? TRUE : FALSE;
    MacOs::ForeignWindow info;
    return ForeignInfo(hwnd, info) && info.onScreen ? TRUE : FALSE;
}

BOOL IsWindowUnicode(HWND) { return TRUE; }

BOOL IsIconic(HWND hwnd) {
    if (MacWin32::IsGameWindow(hwnd)) return MacWin32::GetWindowSnapshot(hwnd).minimized ? TRUE : FALSE;
    MacOs::ForeignWindow info;
    return ForeignInfo(hwnd, info) && !info.onScreen ? TRUE : FALSE;
}

BOOL IsZoomed(HWND hwnd) {
    if (MacWin32::IsGameWindow(hwnd)) {
        const auto s = MacWin32::GetWindowSnapshot(hwnd);
        return (s.zoomed || s.fullscreen) ? TRUE : FALSE;
    }
    return FALSE;
}

BOOL IsChild(HWND, HWND) { return FALSE; }

BOOL GetClientRect(HWND hwnd, LPRECT rect) {
    if (!rect) return FALSE;
    if (MacWin32::IsGameWindow(hwnd)) {
        const auto s = MacWin32::GetWindowSnapshot(hwnd);
        *rect = RECT{ 0, 0, s.clientWidth, s.clientHeight };
        return TRUE;
    }
    MacOs::ForeignWindow info;
    if (ForeignInfo(hwnd, info)) {
        *rect = RECT{ 0, 0, info.bounds.Width(), info.bounds.Height() };
        return TRUE;
    }
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}

BOOL GetWindowRect(HWND hwnd, LPRECT rect) {
    if (!rect) return FALSE;
    if (MacWin32::IsGameWindow(hwnd)) {
        *rect = MacWin32::GetWindowSnapshot(hwnd).frameScreen;
        return TRUE;
    }
    MacOs::ForeignWindow info;
    if (ForeignInfo(hwnd, info)) {
        *rect = ToRect(info.bounds);
        return TRUE;
    }
    if (hwnd == kDesktopWindow || !hwnd) {
        const auto displays = CachedDisplays();
        if (!displays.empty()) {
            *rect = ToRect(displays[0].frame);
            return TRUE;
        }
    }
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}

static bool ClientOrigin(HWND hwnd, POINT& origin) {
    if (MacWin32::IsGameWindow(hwnd)) {
        const RECT r = MacWin32::GetWindowSnapshot(hwnd).clientScreen;
        origin = POINT{ r.left, r.top };
        return true;
    }
    MacOs::ForeignWindow info;
    if (ForeignInfo(hwnd, info)) {
        origin = POINT{ info.bounds.left, info.bounds.top };
        return true;
    }
    if (!hwnd || hwnd == kDesktopWindow) {
        origin = POINT{ 0, 0 };
        return true;
    }
    return false;
}

BOOL ClientToScreen(HWND hwnd, LPPOINT point) {
    POINT origin{};
    if (!point || !ClientOrigin(hwnd, origin)) return FALSE;
    point->x += origin.x;
    point->y += origin.y;
    return TRUE;
}

BOOL ScreenToClient(HWND hwnd, LPPOINT point) {
    POINT origin{};
    if (!point || !ClientOrigin(hwnd, origin)) return FALSE;
    point->x -= origin.x;
    point->y -= origin.y;
    return TRUE;
}

int MapWindowPoints(HWND from, HWND to, LPPOINT points, UINT count) {
    POINT a{}, b{};
    ClientOrigin(from, a);
    ClientOrigin(to, b);
    const LONG dx = a.x - b.x;
    const LONG dy = a.y - b.y;
    for (UINT i = 0; i < count; ++i) {
        points[i].x += dx;
        points[i].y += dy;
    }
    return MAKELONG(static_cast<WORD>(dx), static_cast<WORD>(dy));
}

BOOL AdjustWindowRectEx(LPRECT, DWORD, BOOL, DWORD) { return TRUE; }

HWND GetForegroundWindow(void) {
    HWND game = MacWin32::GameWindow();
    if (game && MacWin32::GetWindowSnapshot(game).key) return game;
    return kDesktopWindow;
}

BOOL SetForegroundWindow(HWND hwnd) {
    if (!MacWin32::IsGameWindow(hwnd)) return FALSE;
    MacOs::WindowActivate(MacWin32::NativeWindow(hwnd));
    return TRUE;
}

HWND GetActiveWindow(void) {
    HWND foreground = GetForegroundWindow();
    return MacWin32::IsGameWindow(foreground) ? foreground : nullptr;
}

HWND GetFocus(void) { return GetActiveWindow(); }
HWND SetFocus(HWND hwnd) { return hwnd; }

HWND GetAncestor(HWND hwnd, UINT) { return hwnd; }
HWND GetParent(HWND) { return nullptr; }
HWND GetWindow(HWND, UINT) { return nullptr; }
HWND GetDesktopWindow(void) { return kDesktopWindow; }

DWORD GetWindowThreadProcessId(HWND hwnd, LPDWORD processId) {
    if (MacWin32::IsGameWindow(hwnd)) {
        if (processId) *processId = GetCurrentProcessId();
        return MacWin32::WindowThreadId();
    }
    MacOs::ForeignWindow info;
    if (ForeignInfo(hwnd, info)) {
        if (processId) *processId = static_cast<DWORD>(info.pid);
        return 1;
    }
    if (processId) *processId = 0;
    return 0;
}

LONG_PTR GetWindowLongPtrW(HWND hwnd, int index) {
    std::lock_guard<std::mutex> lock(g_windowMutex);
    EmulatedWindow* w = FindWindowLocked(hwnd);
    if (!w) return 0;
    switch (index) {
    case GWLP_WNDPROC: return reinterpret_cast<LONG_PTR>(w->proc);
    case GWLP_USERDATA: return w->userData;
    case GWL_STYLE: return w->style;
    case GWL_EXSTYLE: return w->exStyle;
    default: return 0;
    }
}

LONG_PTR SetWindowLongPtrW(HWND hwnd, int index, LONG_PTR value) {
    void* nsWindow = nullptr;
    LONG_PTR previous = 0;
    bool applyBorderless = false;
    bool borderless = false;
    {
        std::lock_guard<std::mutex> lock(g_windowMutex);
        EmulatedWindow* w = FindWindowLocked(hwnd);
        if (!w) {
            SetLastError(ERROR_INVALID_HANDLE);
            return 0;
        }
        switch (index) {
        case GWLP_WNDPROC:
            previous = reinterpret_cast<LONG_PTR>(w->proc);
            w->proc = reinterpret_cast<WNDPROC>(value);
            break;
        case GWLP_USERDATA:
            previous = w->userData;
            w->userData = value;
            break;
        case GWL_STYLE:
            previous = w->style;
            w->style = value;
            nsWindow = w->nsWindow;
            applyBorderless = true;
            borderless = (value & WS_POPUP) != 0 && (value & WS_CAPTION) != WS_CAPTION;
            break;
        case GWL_EXSTYLE:
            previous = w->exStyle;
            w->exStyle = value;
            break;
        default: break;
        }
    }
    if (applyBorderless && nsWindow && !MacOs::WindowIsFullscreen(nsWindow)) MacOs::WindowSetBorderless(nsWindow, borderless);
    SetLastError(ERROR_SUCCESS);
    return previous;
}

LONG GetWindowLongW(HWND hwnd, int index) { return static_cast<LONG>(GetWindowLongPtrW(hwnd, index)); }
LONG SetWindowLongW(HWND hwnd, int index, LONG value) { return static_cast<LONG>(SetWindowLongPtrW(hwnd, index, value)); }

BOOL SetWindowPos(HWND hwnd, HWND, int x, int y, int cx, int cy, UINT flags) {
    void* nsWindow = MacWin32::NativeWindow(hwnd);
    if (!nsWindow) return FALSE;
    if (flags & SWP_HIDEWINDOW) MacOs::WindowMiniaturize(nsWindow);
    if (flags & SWP_SHOWWINDOW) MacOs::WindowRestore(nsWindow);
    if ((flags & SWP_NOSIZE) && (flags & SWP_NOMOVE)) return TRUE;
    if (MacOs::WindowIsFullscreen(nsWindow) && !MacOs::WindowIsBorderless(nsWindow)) return TRUE;
    MacOs::PixelRect current;
    MacOs::WindowContentRect(nsWindow, current);
    MacOs::PixelRect target = current;
    if (!(flags & SWP_NOMOVE)) {
        target.left = x;
        target.top = y;
    }
    const int width = (flags & SWP_NOSIZE) ? current.Width() : cx;
    const int height = (flags & SWP_NOSIZE) ? current.Height() : cy;
    target.right = target.left + width;
    target.bottom = target.top + height;
    MacOs::WindowSetContentRect(nsWindow, target);
    return TRUE;
}

BOOL MoveWindow(HWND hwnd, int x, int y, int width, int height, BOOL) {
    return SetWindowPos(hwnd, nullptr, x, y, width, height, SWP_NOZORDER);
}

BOOL ShowWindow(HWND hwnd, int cmd) {
    void* nsWindow = MacWin32::NativeWindow(hwnd);
    if (!nsWindow) return FALSE;
    const BOOL wasVisible = IsWindowVisible(hwnd);
    switch (cmd) {
    case SW_MINIMIZE:
    case SW_SHOWMINIMIZED:
    case SW_SHOWMINNOACTIVE: MacOs::WindowMiniaturize(nsWindow); break;
    case SW_RESTORE:
    case SW_SHOW:
    case SW_SHOWNORMAL:
    case SW_SHOWNA:
    case SW_SHOWNOACTIVATE: MacOs::WindowRestore(nsWindow); break;
    default: break;
    }
    return wasVisible;
}

BOOL UpdateWindow(HWND) { return TRUE; }
BOOL InvalidateRect(HWND, const RECT*, BOOL) { return TRUE; }
BOOL RedrawWindow(HWND, const RECT*, HRGN, UINT) { return TRUE; }
BOOL DestroyWindow(HWND) { return FALSE; }
ATOM RegisterClassExW(const WNDCLASSEXW*) { return 0; }
BOOL UnregisterClassW(LPCWSTR, HINSTANCE) { return TRUE; }

HWND CreateWindowExW(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID) {
    SetLastError(ERROR_ACCESS_DENIED);
    return nullptr;
}

LRESULT DefWindowProcW(HWND, UINT, WPARAM, LPARAM) { return 0; }

LRESULT CallWindowProcW(WNDPROC proc, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return CallProc(proc, hwnd, msg, wParam, lParam);
}

HANDLE GetPropW(HWND hwnd, LPCWSTR name) {
    if (!name) return nullptr;
    std::lock_guard<std::mutex> lock(g_windowMutex);
    EmulatedWindow* w = FindWindowLocked(hwnd);
    if (!w) return nullptr;
    // GLFW stores its window pointer in the "GLFW" property on Windows.
    if (std::wstring(name) == L"GLFW") return w->hwnd;
    auto it = w->props.find(name);
    return it == w->props.end() ? nullptr : it->second;
}

BOOL SetPropW(HWND hwnd, LPCWSTR name, HANDLE data) {
    if (!name) return FALSE;
    std::lock_guard<std::mutex> lock(g_windowMutex);
    EmulatedWindow* w = FindWindowLocked(hwnd);
    if (!w) return FALSE;
    w->props[name] = data;
    return TRUE;
}

HANDLE RemovePropW(HWND hwnd, LPCWSTR name) {
    if (!name) return nullptr;
    std::lock_guard<std::mutex> lock(g_windowMutex);
    EmulatedWindow* w = FindWindowLocked(hwnd);
    if (!w) return nullptr;
    auto it = w->props.find(name);
    if (it == w->props.end()) return nullptr;
    HANDLE value = it->second;
    w->props.erase(it);
    return value;
}

BOOL EnumWindows(WNDENUMPROC proc, LPARAM lParam) {
    if (!proc) return FALSE;
    if (HWND game = MacWin32::GameWindow()) {
        if (!proc(game, lParam)) return TRUE;
    }
    const uint32_t gameNumber = MacOs::IsMainThread() ? MacOs::WindowNumber(MacWin32::NativeWindow(MacWin32::GameWindow())) : 0;
    for (const MacOs::ForeignWindow& w : MacOs::ListWindows(false)) {
        if (w.layer != 0) continue;
        if (w.id == gameNumber || w.pid == static_cast<int>(GetCurrentProcessId())) continue;
        if (!proc(ForeignHwnd(w.id), lParam)) break;
    }
    return TRUE;
}

static std::string WindowTitleUtf8(HWND hwnd) {
    if (MacWin32::IsGameWindow(hwnd)) return MacWin32::GetWindowSnapshot(hwnd).title;
    MacOs::ForeignWindow info;
    return ForeignInfo(hwnd, info) ? info.title : std::string();
}

int GetWindowTextA(HWND hwnd, LPSTR text, int maxCount) {
    if (!text || maxCount <= 0) return 0;
    const std::string title = WindowTitleUtf8(hwnd);
    const int n = std::min<int>(static_cast<int>(title.size()), maxCount - 1);
    std::memcpy(text, title.data(), static_cast<size_t>(n));
    text[n] = '\0';
    return n;
}

int GetWindowTextW(HWND hwnd, LPWSTR text, int maxCount) {
    if (!text || maxCount <= 0) return 0;
    const std::wstring title = MacWin32::Utf8ToWide(WindowTitleUtf8(hwnd));
    const int n = std::min<int>(static_cast<int>(title.size()), maxCount - 1);
    std::wmemcpy(text, title.data(), static_cast<size_t>(n));
    text[n] = L'\0';
    return n;
}

int GetWindowTextLengthW(HWND hwnd) { return static_cast<int>(MacWin32::Utf8ToWide(WindowTitleUtf8(hwnd)).size()); }
BOOL SetWindowTextW(HWND, LPCWSTR) { return TRUE; }

static std::string ClassNameUtf8(HWND hwnd) {
    if (MacWin32::IsGameWindow(hwnd)) return "GLFW30";
    MacOs::ForeignWindow info;
    // Other applications have no window classes; their name is the closest match.
    return ForeignInfo(hwnd, info) ? info.ownerName : std::string();
}

int GetClassNameA(HWND hwnd, LPSTR className, int maxCount) {
    if (!className || maxCount <= 0) return 0;
    const std::string name = ClassNameUtf8(hwnd);
    const int n = std::min<int>(static_cast<int>(name.size()), maxCount - 1);
    std::memcpy(className, name.data(), static_cast<size_t>(n));
    className[n] = '\0';
    return n;
}

int GetClassNameW(HWND hwnd, LPWSTR className, int maxCount) {
    if (!className || maxCount <= 0) return 0;
    const std::wstring name = MacWin32::Utf8ToWide(ClassNameUtf8(hwnd));
    const int n = std::min<int>(static_cast<int>(name.size()), maxCount - 1);
    std::wmemcpy(className, name.data(), static_cast<size_t>(n));
    className[n] = L'\0';
    return n;
}

HWND WindowFromPoint(POINT point) {
    HWND game = MacWin32::GameWindow();
    if (!game) return nullptr;
    const RECT r = MacWin32::GetWindowSnapshot(game).clientScreen;
    return (point.x >= r.left && point.x < r.right && point.y >= r.top && point.y < r.bottom) ? game : kDesktopWindow;
}

HMONITOR MonitorFromRect(LPCRECT rect, DWORD) {
    if (!rect) return MonitorHandle(0);
    return MonitorHandle(MonitorIndexForRect(*rect));
}

HMONITOR MonitorFromWindow(HWND hwnd, DWORD flags) {
    RECT r{};
    if (!GetWindowRect(hwnd, &r)) return (flags == MONITOR_DEFAULTTONULL) ? nullptr : MonitorHandle(0);
    return MonitorFromRect(&r, flags);
}

HMONITOR MonitorFromPoint(POINT point, DWORD flags) {
    const RECT r{ point.x, point.y, point.x + 1, point.y + 1 };
    return MonitorFromRect(&r, flags);
}

BOOL GetMonitorInfoW(HMONITOR monitor, LPMONITORINFO info) {
    const auto displays = CachedDisplays();
    const size_t index = static_cast<size_t>(reinterpret_cast<uintptr_t>(monitor)) - 1;
    if (!info || index >= displays.size()) return FALSE;
    const MacOs::Display& d = displays[index];
    info->rcMonitor = ToRect(d.frame);
    info->rcWork = ToRect(d.workArea);
    info->dwFlags = d.primary ? MONITORINFOF_PRIMARY : 0;
    if (info->cbSize >= sizeof(MONITORINFOEXW)) {
        MONITORINFOEXW* ex = reinterpret_cast<MONITORINFOEXW*>(info);
        const std::wstring name = L"\\\\.\\DISPLAY" + std::to_wstring(index + 1);
        wcsncpy(ex->szDevice, name.c_str(), CCHDEVICENAME - 1);
        ex->szDevice[CCHDEVICENAME - 1] = L'\0';
    }
    return TRUE;
}

BOOL GetMonitorInfoA(HMONITOR monitor, LPMONITORINFO info) {
    if (!info) return FALSE;
    const DWORD size = info->cbSize;
    info->cbSize = sizeof(MONITORINFO);
    const BOOL ok = GetMonitorInfoW(monitor, info);
    info->cbSize = size;
    return ok;
}

BOOL EnumDisplayMonitors(HDC, LPCRECT, MONITORENUMPROC proc, LPARAM data) {
    if (!proc) return FALSE;
    const auto displays = CachedDisplays();
    for (size_t i = 0; i < displays.size(); ++i) {
        RECT r = ToRect(displays[i].frame);
        if (!proc(MonitorHandle(i), nullptr, &r, data)) break;
    }
    return TRUE;
}

int GetSystemMetrics(int index) {
    const auto displays = CachedDisplays();
    const MacOs::Display primary = displays.empty() ? MacOs::Display{} : displays[0];
    switch (index) {
    case SM_CXSCREEN: return primary.frame.Width();
    case SM_CYSCREEN: return primary.frame.Height();
    case SM_CMONITORS: return static_cast<int>(displays.size());
    case SM_CXCURSOR:
    case SM_CYCURSOR: return MacOs::SystemCursorSizePixels();
    case SM_SWAPBUTTON: return 0;
    case SM_XVIRTUALSCREEN:
    case SM_YVIRTUALSCREEN:
    case SM_CXVIRTUALSCREEN:
    case SM_CYVIRTUALSCREEN: {
        RECT v{ 0, 0, 0, 0 };
        bool first = true;
        for (const auto& d : displays) {
            if (first) {
                v = ToRect(d.frame);
                first = false;
            } else {
                v.left = std::min<LONG>(v.left, d.frame.left);
                v.top = std::min<LONG>(v.top, d.frame.top);
                v.right = std::max<LONG>(v.right, d.frame.right);
                v.bottom = std::max<LONG>(v.bottom, d.frame.bottom);
            }
        }
        if (index == SM_XVIRTUALSCREEN) return v.left;
        if (index == SM_YVIRTUALSCREEN) return v.top;
        if (index == SM_CXVIRTUALSCREEN) return v.right - v.left;
        return v.bottom - v.top;
    }
    default: return 0;
    }
}

int GetSystemMetricsForDpi(int index, UINT dpi) {
    if (index == SM_CXCURSOR || index == SM_CYCURSOR) return static_cast<int>(32 * dpi / USER_DEFAULT_SCREEN_DPI);
    return GetSystemMetrics(index);
}

UINT GetDpiForWindow(HWND hwnd) {
    const double scale = MacWin32::IsGameWindow(hwnd) ? MacWin32::GetWindowSnapshot(hwnd).scale : MacOs::PrimaryDisplayScale();
    return static_cast<UINT>(std::lround(USER_DEFAULT_SCREEN_DPI * scale));
}

UINT GetDpiForSystem(void) { return static_cast<UINT>(std::lround(USER_DEFAULT_SCREEN_DPI * MacOs::PrimaryDisplayScale())); }
BOOL SetProcessDPIAware(void) { return TRUE; }

BOOL SystemParametersInfoW(UINT action, UINT, PVOID data, UINT) {
    switch (action) {
    case SPI_GETMOUSESPEED:
        if (data) *static_cast<int*>(data) = g_mouseSpeed.load();
        return TRUE;
    case SPI_SETMOUSESPEED:
        // The pointer speed setting has no in-game effect on macOS; remember it only.
        g_mouseSpeed.store(static_cast<int>(reinterpret_cast<intptr_t>(data)));
        return TRUE;
    case SPI_GETFILTERKEYS:
        if (data) *static_cast<FILTERKEYS*>(data) = g_filterKeys;
        return TRUE;
    case SPI_SETFILTERKEYS:
        if (data) g_filterKeys = *static_cast<FILTERKEYS*>(data);
        return TRUE;
    case SPI_GETKEYBOARDDELAY: {
        // 0..3 -> 250..1000 ms
        const int ms = MacOs::KeyRepeatInitialDelayMs();
        if (data) *static_cast<int*>(data) = std::clamp((ms - 250 + 125) / 250, 0, 3);
        return TRUE;
    }
    case SPI_GETKEYBOARDSPEED: {
        // 0..31 -> ~2.5..30 repeats per second
        const int ms = std::max(1, MacOs::KeyRepeatIntervalMs());
        const double rate = 1000.0 / ms;
        if (data) *static_cast<int*>(data) = std::clamp(static_cast<int>(std::lround((rate - 2.5) * 31.0 / 27.5)), 0, 31);
        return TRUE;
    }
    case SPI_GETWORKAREA: {
        const auto displays = CachedDisplays();
        if (data && !displays.empty()) *static_cast<RECT*>(data) = ToRect(displays[0].workArea);
        return TRUE;
    }
    default: return FALSE;
    }
}

BOOL SystemParametersInfoA(UINT action, UINT param, PVOID data, UINT winIni) { return SystemParametersInfoW(action, param, data, winIni); }

int MessageBoxW(HWND, LPCWSTR text, LPCWSTR caption, UINT type) {
    std::vector<std::string> buttons;
    const UINT kind = type & 0xF;
    if (kind == MB_OKCANCEL) buttons = { "OK", "Cancel" };
    else if (kind == MB_YESNO) buttons = { "Yes", "No" };
    else buttons = { "OK" };
    const bool critical = (type & 0xF0) == MB_ICONERROR || (type & 0xF0) == MB_ICONWARNING;
    const int pressed = MacOs::ShowAlert(ToUtf8(caption), ToUtf8(text), buttons, critical);
    if (kind == MB_OKCANCEL) return pressed == 0 ? IDOK : IDCANCEL;
    if (kind == MB_YESNO) return pressed == 0 ? IDYES : IDNO;
    return IDOK;
}

int MessageBoxA(HWND hwnd, LPCSTR text, LPCSTR caption, UINT type) {
    const std::wstring t = MacWin32::Utf8ToWide(text ? text : "");
    const std::wstring c = MacWin32::Utf8ToWide(caption ? caption : "");
    return MessageBoxW(hwnd, t.c_str(), c.c_str(), type);
}

BOOL FlashWindow(HWND, BOOL) { return TRUE; }

// --- messages ------------------------------------------------------------------------------------------

BOOL PostMessageW(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!hwnd) return PostThreadMessageW(GetCurrentThreadId(), msg, wParam, lParam);
    if (!MacWin32::IsGameWindow(hwnd)) {
        // Other applications' windows cannot receive messages from this process.
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    std::lock_guard<std::mutex> lock(g_postMutex);
    g_posted.push_back(PostedMessage{ hwnd, msg, wParam, lParam });
    return TRUE;
}

LRESULT SendMessageW(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!MacWin32::IsGameWindow(hwnd)) return 0;
    if (GetCurrentThreadId() == MacWin32::WindowThreadId()) return MacWin32::DispatchToWindow(hwnd, msg, wParam, lParam);
    // Cross-thread sends are delivered on the window thread at its next pump.
    PostMessageW(hwnd, msg, wParam, lParam);
    return 0;
}

LRESULT SendMessageTimeoutW(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT, UINT, PDWORD_PTR result) {
    if (!MacWin32::IsGameWindow(hwnd)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return 0;
    }
    const LRESULT r = SendMessageW(hwnd, msg, wParam, lParam);
    if (result) *result = static_cast<DWORD_PTR>(r);
    return 1;
}

BOOL SendNotifyMessageW(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) { return PostMessageW(hwnd, msg, wParam, lParam); }

BOOL PostThreadMessageW(DWORD threadId, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto q = QueueFor(threadId, false);
    if (!q) {
        SetLastError(1444); // ERROR_INVALID_THREAD_ID
        return FALSE;
    }
    {
        std::lock_guard<std::mutex> lock(q->mutex);
        MSG m{};
        m.message = msg;
        m.wParam = wParam;
        m.lParam = lParam;
        m.time = GetTickCount();
        q->messages.push_back(m);
    }
    q->cv.notify_all();
    return TRUE;
}

static bool MatchesFilter(const MSG& m, UINT filterMin, UINT filterMax) {
    if (filterMin == 0 && filterMax == 0) return true;
    return m.message >= filterMin && m.message <= filterMax;
}

BOOL GetMessageW(LPMSG msg, HWND, UINT filterMin, UINT filterMax) {
    auto q = QueueFor(GetCurrentThreadId(), true);
    std::unique_lock<std::mutex> lock(q->mutex);
    for (;;) {
        for (auto it = q->messages.begin(); it != q->messages.end(); ++it) {
            if (MatchesFilter(*it, filterMin, filterMax)) {
                *msg = *it;
                q->messages.erase(it);
                return msg->message == WM_QUIT ? FALSE : TRUE;
            }
        }
        q->cv.wait(lock);
    }
}

BOOL PeekMessageW(LPMSG msg, HWND, UINT filterMin, UINT filterMax, UINT removeMsg) {
    auto q = QueueFor(GetCurrentThreadId(), true);
    std::lock_guard<std::mutex> lock(q->mutex);
    for (auto it = q->messages.begin(); it != q->messages.end(); ++it) {
        if (MatchesFilter(*it, filterMin, filterMax)) {
            if (msg) *msg = *it;
            if (removeMsg & PM_REMOVE) q->messages.erase(it);
            return TRUE;
        }
    }
    return FALSE;
}

BOOL TranslateMessage(const MSG*) { return FALSE; }

LRESULT DispatchMessageW(const MSG* msg) {
    if (!msg || !msg->hwnd) return 0;
    return MacWin32::DispatchToWindow(msg->hwnd, msg->message, msg->wParam, msg->lParam);
}

void PostQuitMessage(int exitCode) { PostThreadMessageW(GetCurrentThreadId(), WM_QUIT, static_cast<WPARAM>(exitCode), 0); }

UINT RegisterWindowMessageA(LPCSTR name) {
    if (!name) return 0;
    std::lock_guard<std::mutex> lock(g_registeredMessagesMutex);
    auto it = g_registeredMessages.find(name);
    if (it != g_registeredMessages.end()) return it->second;
    const UINT id = 0xC000 + static_cast<UINT>(g_registeredMessages.size());
    g_registeredMessages[name] = id;
    return id;
}

UINT RegisterWindowMessageW(LPCWSTR name) { return RegisterWindowMessageA(ToUtf8(name).c_str()); }

UINT_PTR SetTimer(HWND hwnd, UINT_PTR id, UINT elapse, void*) {
    if (!MacWin32::IsGameWindow(hwnd)) return 0;
    std::lock_guard<std::mutex> lock(g_timerMutex);
    for (Timer& t : g_timers) {
        if (t.hwnd == hwnd && t.id == id) {
            t.elapse = elapse;
            t.nextMs = NowMs() + std::max<UINT>(elapse, 1);
            return id;
        }
    }
    g_timers.push_back(Timer{ hwnd, id, elapse, NowMs() + std::max<UINT>(elapse, 1) });
    return id;
}

BOOL KillTimer(HWND hwnd, UINT_PTR id) {
    std::lock_guard<std::mutex> lock(g_timerMutex);
    const auto before = g_timers.size();
    g_timers.erase(std::remove_if(g_timers.begin(), g_timers.end(), [&](const Timer& t) { return t.hwnd == hwnd && t.id == id; }),
                   g_timers.end());
    return g_timers.size() != before;
}

LPARAM GetMessageExtraInfo(void) { return t_messageExtraInfo; }
LONG GetMessageTime(void) { return t_dispatchDepth > 0 ? t_messageTime : static_cast<LONG>(GetTickCount()); }
DWORD GetQueueStatus(UINT) { return 0; }

// --- keyboard / mouse ------------------------------------------------------------------------------------

SHORT GetAsyncKeyState(int vk) {
    if (vk <= 0 || vk > 255) return 0;
    return g_keyDown[vk].load() ? static_cast<SHORT>(0x8000) : 0;
}

SHORT GetKeyState(int vk) {
    if (vk <= 0 || vk > 255) return 0;
    SHORT state = g_keyDown[vk].load() ? static_cast<SHORT>(0x8000) : 0;
    if (vk == VK_CAPITAL) {
        if (MacOs::CapsLockOn()) state |= 1;
    } else if (vk == VK_NUMLOCK) {
        state |= 1; // Mac keypads always type digits.
    } else if (g_keyToggled[vk].load()) {
        state |= 1;
    }
    return state;
}

BOOL GetKeyboardState(PBYTE keyState) {
    if (!keyState) return FALSE;
    for (int vk = 0; vk < 256; ++vk) {
        const SHORT s = GetKeyState(vk);
        keyState[vk] = static_cast<BYTE>(((s & 0x8000) ? 0x80 : 0) | (s & 1));
    }
    return TRUE;
}

BOOL SetKeyboardState(PBYTE) { return TRUE; }

UINT MapVirtualKeyExW(UINT code, UINT mapType, HKL) {
    switch (mapType) {
    case MAPVK_VK_TO_VSC: {
        const unsigned scan = MacKeymap::ScanCodeFromVk(code);
        return scan & 0xFF;
    }
    case MAPVK_VK_TO_VSC_EX: {
        const unsigned scan = MacKeymap::ScanCodeFromVk(code);
        return (scan & MacKeymap::kExtended) ? (0xE000 | (scan & 0xFF)) : scan;
    }
    case MAPVK_VSC_TO_VK:
    case MAPVK_VSC_TO_VK_EX: {
        unsigned scan = code & 0xFF;
        if ((code & 0xFF00) == 0xE000 || (code & 0xFF00) == 0xE100) scan |= MacKeymap::kExtended;
        return MacKeymap::VkFromScanCode(scan, mapType == MAPVK_VSC_TO_VK_EX);
    }
    case MAPVK_VK_TO_CHAR: {
        if (code >= 'A' && code <= 'Z') return code;
        if (code >= '0' && code <= '9') return code;
        const int mac = MacKeymap::MacKeyCodeFromVk(code);
        if (mac < 0) return 0;
        const std::u16string chars = MacOs::TranslateKey(static_cast<uint16_t>(mac), false, false, false, false);
        if (chars.empty()) return 0;
        UINT c = chars[0];
        if (c >= 'a' && c <= 'z') c -= 32;
        return c;
    }
    default: return 0;
    }
}

UINT MapVirtualKeyW(UINT code, UINT mapType) { return MapVirtualKeyExW(code, mapType, nullptr); }
UINT MapVirtualKeyA(UINT code, UINT mapType) { return MapVirtualKeyExW(code, mapType, nullptr); }

int ToUnicodeEx(UINT vk, UINT scanCode, const BYTE* keyState, LPWSTR buffer, int bufferSize, UINT, HKL) {
    if (!buffer || bufferSize <= 0) return 0;
    int mac = -1;
    if (scanCode != 0) mac = MacKeymap::MacKeyCodeFromScanCode(scanCode & 0x1FF);
    if (mac < 0) mac = MacKeymap::MacKeyCodeFromVk(vk);
    if (mac < 0) return 0;
    const bool shift = keyState && (keyState[VK_SHIFT] & 0x80);
    const bool caps = keyState && (keyState[VK_CAPITAL] & 0x01);
    const bool ctrl = keyState && (keyState[VK_CONTROL] & 0x80);
    const bool alt = keyState && (keyState[VK_MENU] & 0x80);
    // Windows treats Ctrl+Alt as AltGr; the Mac equivalent is Option.
    const bool option = alt;
    const bool control = ctrl && !alt;
    const std::u16string chars = MacOs::TranslateKey(static_cast<uint16_t>(mac), shift, option, caps, control);
    if (chars.empty()) return 0;
    int written = 0;
    for (char16_t c : chars) {
        if (written >= bufferSize) break;
        buffer[written++] = static_cast<wchar_t>(c);
    }
    if (written < bufferSize) buffer[written] = L'\0';
    return written;
}

int ToUnicode(UINT vk, UINT scanCode, const BYTE* keyState, LPWSTR buffer, int bufferSize, UINT flags) {
    return ToUnicodeEx(vk, scanCode, keyState, buffer, bufferSize, flags, nullptr);
}

SHORT VkKeyScanExW(WCHAR ch, HKL) {
    for (int shift = 0; shift < 2; ++shift) {
        for (int option = 0; option < 2; ++option) {
            for (int mac = 0; mac < 128; ++mac) {
                const std::u16string chars = MacOs::TranslateKey(static_cast<uint16_t>(mac), shift != 0, option != 0, false, false);
                if (chars.size() == 1 && chars[0] == static_cast<char16_t>(ch)) {
                    const unsigned vk = MacKeymap::VkFromMacKeyCode(mac);
                    if (vk == 0) continue;
                    // High byte: 1 = Shift, 6 = Ctrl+Alt (AltGr / Option).
                    const int modifiers = (shift ? 1 : 0) | (option ? 6 : 0);
                    return static_cast<SHORT>((modifiers << 8) | (vk & 0xFF));
                }
            }
        }
    }
    return -1;
}

SHORT VkKeyScanW(WCHAR ch) { return VkKeyScanExW(ch, nullptr); }
SHORT VkKeyScanA(CHAR ch) { return VkKeyScanExW(static_cast<WCHAR>(static_cast<unsigned char>(ch)), nullptr); }

int GetKeyNameTextA(LONG lParam, LPSTR buffer, int size) {
    if (!buffer || size <= 0) return 0;
    unsigned scan = (static_cast<unsigned>(lParam) >> 16) & 0xFF;
    if (lParam & (1 << 24)) scan |= MacKeymap::kExtended;
    const std::string name = MacKeymap::KeyName(scan);
    const int n = std::min<int>(static_cast<int>(name.size()), size - 1);
    std::memcpy(buffer, name.data(), static_cast<size_t>(n));
    buffer[n] = '\0';
    return n;
}

int GetKeyNameTextW(LONG lParam, LPWSTR buffer, int size) {
    char narrow[128];
    GetKeyNameTextA(lParam, narrow, sizeof(narrow));
    const std::wstring wide = MacWin32::Utf8ToWide(narrow);
    if (!buffer || size <= 0) return 0;
    const int n = std::min<int>(static_cast<int>(wide.size()), size - 1);
    std::wmemcpy(buffer, wide.data(), static_cast<size_t>(n));
    buffer[n] = L'\0';
    return n;
}

HKL GetKeyboardLayout(DWORD) { return reinterpret_cast<HKL>(MacKeymap::KeyboardLayoutHandle()); }

UINT SendInput(UINT count, LPINPUT inputs, int) {
    if (!inputs) return 0;
    std::lock_guard<std::mutex> lock(g_injectMutex);
    for (UINT i = 0; i < count; ++i) g_injected.push_back(inputs[i]);
    return count;
}

void keybd_event(BYTE vk, BYTE scan, DWORD flags, ULONG_PTR extraInfo) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = scan;
    in.ki.dwFlags = flags;
    in.ki.dwExtraInfo = extraInfo;
    SendInput(1, &in, sizeof(INPUT));
}

HHOOK SetWindowsHookExW(int idHook, HOOKPROC proc, HINSTANCE, DWORD) {
    if (!proc || (idHook != WH_KEYBOARD_LL && idHook != WH_MOUSE_LL)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(g_hookMutex);
    HHOOK id = reinterpret_cast<HHOOK>(g_nextHookId.fetch_add(1));
    g_hooks.push_back(LowLevelHook{ id, idHook, proc });
    return id;
}

BOOL UnhookWindowsHookEx(HHOOK hook) {
    std::lock_guard<std::mutex> lock(g_hookMutex);
    const auto before = g_hooks.size();
    g_hooks.erase(std::remove_if(g_hooks.begin(), g_hooks.end(), [&](const LowLevelHook& h) { return h.id == hook; }), g_hooks.end());
    return g_hooks.size() != before;
}

LRESULT CallNextHookEx(HHOOK, int, WPARAM, LPARAM) { return 0; }

BOOL GetCursorPos(LPPOINT point) {
    if (!point) return FALSE;
    int x = 0, y = 0;
    if (!MacOs::CursorPosition(x, y)) return FALSE;
    point->x = x;
    point->y = y;
    return TRUE;
}

BOOL GetPhysicalCursorPos(LPPOINT point) { return GetCursorPos(point); }

BOOL SetCursorPos(int x, int y) {
    std::function<void(int, int)> warp;
    {
        std::lock_guard<std::mutex> lock(g_cursorMutex);
        warp = g_cursorCallbacks.warp;
    }
    if (warp) warp(x, y);
    else MacOs::WarpCursor(x, y);
    return TRUE;
}

BOOL ClipCursor(const RECT* rect) {
    std::lock_guard<std::mutex> lock(g_cursorMutex);
    g_haveClip = rect != nullptr;
    if (rect) g_clip = *rect;
    return TRUE;
}

BOOL GetClipCursor(LPRECT rect) {
    if (!rect) return FALSE;
    std::lock_guard<std::mutex> lock(g_cursorMutex);
    if (g_haveClip) {
        *rect = g_clip;
        return TRUE;
    }
    rect->left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    rect->top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    rect->right = rect->left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    rect->bottom = rect->top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return TRUE;
}

HCURSOR SetCursor(HCURSOR cursor) {
    const HCURSOR previous = g_currentCursor.exchange(cursor);
    if (!cursor) {
        g_cursorHiddenBySetCursor.store(true);
    } else {
        g_cursorHiddenBySetCursor.store(false);
        if (void* native = MacGdi::NativeCursor(cursor)) MacOs::SetCursor(native);
    }
    NotifyCursorVisibility();
    return previous;
}

HCURSOR GetCursor(void) { return g_currentCursor.load(); }

int ShowCursor(BOOL show) {
    const int count = show ? g_showCount.fetch_add(1) + 1 : g_showCount.fetch_sub(1) - 1;
    NotifyCursorVisibility();
    return count;
}

BOOL GetCursorInfo(PCURSORINFO info) {
    if (!info) return FALSE;
    const bool visible = MacWin32::IsWin32CursorVisible();
    info->flags = visible ? CURSOR_SHOWING : 0;
    HCURSOR cursor = g_currentCursor.load();
    info->hCursor = visible ? (cursor ? cursor : MacGdi::ArrowCursor()) : nullptr;
    GetCursorPos(&info->ptScreenPos);
    return TRUE;
}

HWND SetCapture(HWND hwnd) { return g_capture.exchange(hwnd); }

BOOL ReleaseCapture(void) {
    g_capture.store(nullptr);
    return TRUE;
}

HWND GetCapture(void) { return g_capture.load(); }

BOOL TrackMouseEvent(LPTRACKMOUSEEVENT eventTrack) {
    if (!eventTrack) return FALSE;
    if (eventTrack->dwFlags & TME_CANCEL) g_trackLeave.store(false);
    else if (eventTrack->dwFlags & TME_LEAVE) g_trackLeave.store(true);
    return TRUE;
}

UINT GetDoubleClickTime(void) { return 500; }
UINT GetCaretBlinkTime(void) { return 530; }
BOOL SwapMouseButton(BOOL) { return FALSE; }
UINT GetRawInputData(HRAWINPUT, UINT, LPVOID, PUINT, UINT) { return static_cast<UINT>(-1); }

// --- clipboard ---------------------------------------------------------------------------------------------

BOOL OpenClipboard(HWND) { return TRUE; }

BOOL CloseClipboard(void) {
    std::string text;
    bool set = false;
    {
        std::lock_guard<std::mutex> lock(g_clipboardMutex);
        set = g_pendingClipboardSet;
        text = g_pendingClipboard;
        g_pendingClipboardSet = false;
    }
    if (set) MacOs::SetClipboardText(text);
    return TRUE;
}

BOOL EmptyClipboard(void) {
    std::lock_guard<std::mutex> lock(g_clipboardMutex);
    g_pendingClipboard.clear();
    g_pendingClipboardSet = false;
    return TRUE;
}

HANDLE SetClipboardData(UINT format, HANDLE mem) {
    const void* data = GlobalLock(mem);
    const SIZE_T size = GlobalSize(mem);
    if (!data) return nullptr;
    std::string text;
    if (format == CF_UNICODETEXT) {
        const wchar_t* w = static_cast<const wchar_t*>(data);
        text = MacWin32::WideToUtf8(std::wstring(w, wcsnlen(w, size / sizeof(wchar_t))));
    } else if (format == CF_TEXT) {
        const char* c = static_cast<const char*>(data);
        text.assign(c, strnlen(c, size));
    } else {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(g_clipboardMutex);
    g_pendingClipboard = text;
    g_pendingClipboardSet = true;
    return mem;
}

HANDLE GetClipboardData(UINT format) {
    std::string text;
    if (!MacOs::GetClipboardText(text)) return nullptr;
    if (format == CF_UNICODETEXT) {
        const std::wstring w = MacWin32::Utf8ToWide(text);
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
        std::wmemcpy(static_cast<wchar_t*>(GlobalLock(mem)), w.c_str(), w.size() + 1);
        return mem;
    }
    if (format == CF_TEXT) {
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
        std::memcpy(GlobalLock(mem), text.c_str(), text.size() + 1);
        return mem;
    }
    return nullptr;
}

BOOL IsClipboardFormatAvailable(UINT format) {
    std::string text;
    return (format == CF_TEXT || format == CF_UNICODETEXT) && MacOs::GetClipboardText(text);
}

// --- common dialogs ----------------------------------------------------------------------------------------

static std::vector<std::string> ParseFilterExtensions(LPCWSTR filter, DWORD index) {
    std::vector<std::string> extensions;
    if (!filter) return extensions;
    // "Description\0*.png;*.jpg\0Description\0*.*\0\0"
    std::vector<std::wstring> parts;
    const wchar_t* p = filter;
    while (*p) {
        parts.emplace_back(p);
        p += parts.back().size() + 1;
    }
    const size_t pair = index > 0 ? index - 1 : 0;
    if (pair * 2 + 1 >= parts.size()) return extensions;
    std::string pattern = MacWin32::WideToUtf8(parts[pair * 2 + 1]);
    size_t start = 0;
    while (start <= pattern.size()) {
        const size_t end = pattern.find(';', start);
        std::string item = pattern.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const size_t dot = item.rfind('.');
        if (dot != std::string::npos) {
            std::string ext = item.substr(dot + 1);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == "*") return {};
            if (!ext.empty()) extensions.push_back(ext);
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return extensions;
}

static BOOL RunFileDialog(LPOPENFILENAMEW ofn, bool save) {
    if (!ofn || !ofn->lpstrFile || ofn->nMaxFile == 0) return FALSE;
    std::string initial = ofn->lpstrInitialDir ? MacWin32::WideToUtf8(ofn->lpstrInitialDir) : std::string();
    std::replace(initial.begin(), initial.end(), '\\', '/');
    const std::string title = ofn->lpstrTitle ? MacWin32::WideToUtf8(ofn->lpstrTitle) : std::string();
    std::string chosen;
    bool ok = false;
    if (save) {
        ok = MacOs::ChooseSaveFile(title, initial, MacWin32::WideToUtf8(ofn->lpstrFile), chosen);
    } else {
        ok = MacOs::ChooseFile(title, initial, ParseFilterExtensions(ofn->lpstrFilter, ofn->nFilterIndex), chosen);
    }
    HWND game = MacWin32::GameWindow();
    if (game) MacOs::WindowActivate(MacWin32::NativeWindow(game));
    if (!ok) return FALSE;
    const std::wstring wide = MacWin32::Utf8ToWide(chosen);
    if (wide.size() >= ofn->nMaxFile) return FALSE;
    std::wmemcpy(ofn->lpstrFile, wide.c_str(), wide.size() + 1);
    const size_t slash = wide.find_last_of(L'/');
    ofn->nFileOffset = static_cast<WORD>(slash == std::wstring::npos ? 0 : slash + 1);
    const size_t dot = wide.find_last_of(L'.');
    ofn->nFileExtension = static_cast<WORD>((dot == std::wstring::npos || dot < ofn->nFileOffset) ? 0 : dot + 1);
    return TRUE;
}

BOOL GetOpenFileNameW(LPOPENFILENAMEW ofn) { return RunFileDialog(ofn, false); }
BOOL GetSaveFileNameW(LPOPENFILENAMEW ofn) { return RunFileDialog(ofn, true); }
DWORD CommDlgExtendedError(void) { return 0; }

// --- dwm -------------------------------------------------------------------------------------------------

HRESULT DwmGetWindowAttribute(HWND hwnd, DWORD attribute, PVOID value, DWORD size) {
    if (!value) return E_INVALIDARG;
    if (attribute == DWMWA_CLOAKED && size >= sizeof(DWORD)) {
        *static_cast<DWORD*>(value) = 0;
        return S_OK;
    }
    if (attribute == DWMWA_EXTENDED_FRAME_BOUNDS && size >= sizeof(RECT)) {
        return GetWindowRect(hwnd, static_cast<RECT*>(value)) ? S_OK : E_FAIL;
    }
    return E_FAIL;
}

HRESULT DwmEnableBlurBehindWindow(HWND, const DWM_BLURBEHIND*) { return S_OK; }

HRESULT DwmIsCompositionEnabled(BOOL* enabled) {
    if (enabled) *enabled = TRUE;
    return S_OK;
}

HRESULT DwmGetColorizationColor(DWORD*, BOOL*) { return E_FAIL; }

// --- wgl ---------------------------------------------------------------------------------------------------

HGLRC wglGetCurrentContext(void) { return static_cast<HGLRC>(CGLGetCurrentContext()); }

HDC wglGetCurrentDC(void) {
    HWND game = MacWin32::GameWindow();
    return game ? GetDC(game) : nullptr;
}

FARPROC wglGetProcAddress(LPCSTR) { return nullptr; }
BOOL wglMakeCurrent(HDC, HGLRC) { return FALSE; }
BOOL SwapBuffers(HDC) { return FALSE; }

} // extern "C"
