#pragma once

// Plain C++ interface over the macOS services the Win32 layer needs. The
// implementation (mac_os.mm) is the only place that includes Cocoa/Carbon, so
// Win32 and Apple type names never meet in one translation unit.
//
// "Screen pixels" are global coordinates in device pixels with the origin at
// the top-left corner of the primary display, matching Win32 screen space.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace MacOs {

struct PixelRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    int Width() const { return right - left; }
    int Height() const { return bottom - top; }
};

// Scale from points to "screen pixels" (the game display's backing scale).
void SetCoordinateScale(double scale);

// Re-reads the keyboard layout. Main thread only; other threads use the cache.
void RefreshKeyboardLayout();

// --- main thread -------------------------------------------------------------
// Functions below that touch AppKit run on the main thread (synchronously when
// called elsewhere). Callers off the main thread should prefer cached state.
bool IsMainThread();
// Runs fn on the main thread and waits for it (runs inline on the main thread).
void RunOnMainThreadSync(const std::function<void()>& fn);
void RunOnMainThreadAsync(std::function<void()> fn);

// --- displays ------------------------------------------------------------------
struct Display {
    PixelRect frame;
    PixelRect workArea;
    double scale = 1.0;
    bool primary = false;
    uint32_t id = 0;
    std::string name;
};
std::vector<Display> Displays();
double PrimaryDisplayScale();

// --- windows (NSWindow* passed as void*) -------------------------------------------
bool WindowContentRect(void* nsWindow, PixelRect& out);
bool WindowFrameRect(void* nsWindow, PixelRect& out);
double WindowBackingScale(void* nsWindow);
bool WindowIsKey(void* nsWindow);
bool WindowIsVisible(void* nsWindow);
bool WindowIsMiniaturized(void* nsWindow);
bool WindowIsZoomed(void* nsWindow);
bool WindowIsFullscreen(void* nsWindow);
bool ApplicationIsActive();
void WindowActivate(void* nsWindow);
uint32_t WindowNumber(void* nsWindow);
std::string WindowTitle(void* nsWindow);
// Moves/resizes the window so its content area covers `rect` (screen pixels).
void WindowSetContentRect(void* nsWindow, const PixelRect& rect);
// Toggles a borderless window covering the display (Toolscreen's borderless mode).
void WindowSetBorderless(void* nsWindow, bool borderless);
bool WindowIsBorderless(void* nsWindow);
void WindowMiniaturize(void* nsWindow);
void WindowRestore(void* nsWindow);

// --- other applications' windows ----------------------------------------------------
struct ForeignWindow {
    uint32_t id = 0;
    int pid = 0;
    int layer = 0;
    bool onScreen = false;
    std::string title;
    std::string ownerName;
    std::string ownerPath;
    PixelRect bounds;
};
std::vector<ForeignWindow> ListWindows(bool onScreenOnly);
bool WindowInfo(uint32_t windowId, ForeignWindow& out);

// --- cursor ------------------------------------------------------------------------
bool CursorPosition(int& x, int& y);
void WarpCursor(int x, int y);
void SetCursorAssociated(bool associated);
// NSCursor objects are returned retained; release with ReleaseCursor.
void* CreateCursor(const uint8_t* rgba, int width, int height, int hotX, int hotY);
enum class SystemCursorKind { Arrow, IBeam, Wait, Cross, Hand, ResizeEW, ResizeNS, ResizeNWSE, ResizeNESW, ResizeAll, NotAllowed };
void* SystemCursor(SystemCursorKind kind);
void ReleaseCursor(void* cursor);
void SetCursor(void* cursor);
void HideCursor();
void UnhideCursor();
bool CursorImage(void* cursor, std::vector<uint8_t>& rgba, int& width, int& height, int& hotX, int& hotY);
int SystemCursorSizePixels();

// --- keyboard ------------------------------------------------------------------------
// Characters produced by `keyCode` in the current layout (no dead-key state).
std::u16string TranslateKey(uint16_t keyCode, bool shift, bool option, bool capsLock, bool control);
// Identifier that changes whenever the input source changes.
uint64_t KeyboardLayoutId();
// Primary language of the current input source, e.g. "en", "de".
std::string KeyboardLayoutLanguage();
std::string KeyboardLayoutName();
// Physical keyboard type: 0 = ANSI, 1 = ISO, 2 = JIS.
int PhysicalKeyboardType();
bool CapsLockOn();
// Key repeat timings from the user's settings, in milliseconds.
int KeyRepeatInitialDelayMs();
int KeyRepeatIntervalMs();

// --- clipboard / shell / dialogs ----------------------------------------------------------
bool SetClipboardText(const std::string& utf8);
bool GetClipboardText(std::string& utf8);
bool OpenUrl(const std::string& urlOrPath);
// Blocking file chooser. extensions: lowercase, without dots; empty = any file.
bool ChooseFile(const std::string& title, const std::string& initialDirectory, const std::vector<std::string>& extensions,
                std::string& outPath);
bool ChooseSaveFile(const std::string& title, const std::string& initialDirectory, const std::string& defaultName,
                    std::string& outPath);
// Returns the index of the pressed button.
int ShowAlert(const std::string& title, const std::string& text, const std::vector<std::string>& buttons, bool critical);

// --- paths ----------------------------------------------------------------------------
std::string HomeDirectory();
std::string TemporaryDirectory();
std::string DownloadsDirectory();
std::string DocumentsDirectory();
std::string DesktopDirectory();
std::string PicturesDirectory();
std::string ApplicationSupportDirectory();
std::vector<std::string> FontDirectories();

// --- processes ------------------------------------------------------------------------
// Process start time as microseconds since the Unix epoch.
bool ProcessStartTimeUs(int pid, uint64_t& outUs);
bool ProcessAlive(int pid);
std::string ProcessPath(int pid);
std::vector<std::string> CommandLineArguments();

// --- HTTP -----------------------------------------------------------------------------
struct HttpResponse {
    int status = 0;
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<uint8_t> body;
    std::string error;
};
bool HttpRequest(const std::string& method, const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
                 const std::vector<uint8_t>& body, int timeoutMs, HttpResponse& out);

} // namespace MacOs
