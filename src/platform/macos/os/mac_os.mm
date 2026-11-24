#import <Cocoa/Cocoa.h>
#import <Carbon/Carbon.h>
#import <CoreGraphics/CoreGraphics.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "platform/macos/os/mac_os.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <crt_externs.h>
#include <libproc.h>
#include <mutex>
#include <pthread.h>
#include <signal.h>
#include <sys/sysctl.h>

namespace {

std::atomic<double> g_coordinateScale{ 0.0 };

double CoordinateScale() {
    double s = g_coordinateScale.load(std::memory_order_relaxed);
    if (s > 0.0) return s;
    return MacOs::PrimaryDisplayScale();
}

double PrimaryHeightPoints() {
    CGRect bounds = CGDisplayBounds(CGMainDisplayID());
    return bounds.size.height;
}

// Cocoa screen coordinates (points, bottom-left origin) -> screen pixels.
MacOs::PixelRect CocoaRectToPixels(NSRect r) {
    const double s = CoordinateScale();
    const double top = PrimaryHeightPoints() - (r.origin.y + r.size.height);
    MacOs::PixelRect out;
    out.left = static_cast<int>(std::lround(r.origin.x * s));
    out.top = static_cast<int>(std::lround(top * s));
    out.right = static_cast<int>(std::lround((r.origin.x + r.size.width) * s));
    out.bottom = static_cast<int>(std::lround((top + r.size.height) * s));
    return out;
}

// CoreGraphics global coordinates (points, top-left origin) -> screen pixels.
MacOs::PixelRect CGRectToPixels(CGRect r) {
    const double s = CoordinateScale();
    MacOs::PixelRect out;
    out.left = static_cast<int>(std::lround(r.origin.x * s));
    out.top = static_cast<int>(std::lround(r.origin.y * s));
    out.right = static_cast<int>(std::lround((r.origin.x + r.size.width) * s));
    out.bottom = static_cast<int>(std::lround((r.origin.y + r.size.height) * s));
    return out;
}

NSRect PixelsToCocoaRect(const MacOs::PixelRect& r) {
    const double s = CoordinateScale();
    const double x = r.left / s;
    const double width = r.Width() / s;
    const double height = r.Height() / s;
    const double top = r.top / s;
    return NSMakeRect(x, PrimaryHeightPoints() - top - height, width, height);
}

NSString* ToNSString(const std::string& s) { return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding] ?: @""; }

std::string ToStdString(NSString* s) {
    if (!s) return {};
    const char* utf8 = [s UTF8String];
    return utf8 ? std::string(utf8) : std::string();
}

// Runs on the main thread inside an autorelease pool. The game's main thread
// has no pool of its own outside GLFW's event polling.
template <typename F> auto OnMain(F&& fn) -> decltype(fn()) {
    if ([NSThread isMainThread]) {
        @autoreleasepool {
            return fn();
        }
    }
    __block decltype(fn()) result{};
    dispatch_sync(dispatch_get_main_queue(), ^{
      @autoreleasepool {
          result = fn();
      }
    });
    return result;
}

// Keyboard layout data is copied on the main thread (TIS requires it) and used
// from any thread afterwards.
struct LayoutCache {
    std::mutex mutex;
    CFDataRef layoutData = nullptr;
    uint64_t id = 0;
    std::string language;
    std::string name;
    UInt32 keyboardType = 0;
    bool loaded = false;
};
LayoutCache g_layout;
std::atomic<uint64_t> g_layoutGeneration{ 1 };

void LoadLayoutOnMainThread() {
    @autoreleasepool {
    TISInputSourceRef source = TISCopyCurrentKeyboardLayoutInputSource();
    if (!source) return;
    CFDataRef data = static_cast<CFDataRef>(TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData));
    if (!data) {
        CFRelease(source);
        source = TISCopyCurrentASCIICapableKeyboardLayoutInputSource();
        if (!source) return;
        data = static_cast<CFDataRef>(TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData));
    }
    NSString* sourceId = (__bridge NSString*)TISGetInputSourceProperty(source, kTISPropertyInputSourceID);
    NSString* localizedName = (__bridge NSString*)TISGetInputSourceProperty(source, kTISPropertyLocalizedName);
    NSArray* languages = (__bridge NSArray*)TISGetInputSourceProperty(source, kTISPropertyInputSourceLanguages);

    std::lock_guard<std::mutex> lock(g_layout.mutex);
    if (g_layout.layoutData) CFRelease(g_layout.layoutData);
    g_layout.layoutData = data ? static_cast<CFDataRef>(CFRetain(data)) : nullptr;
    g_layout.name = ToStdString(localizedName);
    g_layout.language = (languages.count > 0) ? ToStdString(languages[0]) : std::string("en");
    g_layout.keyboardType = LMGetKbdType();
    const std::string idString = ToStdString(sourceId);
    g_layout.id = std::hash<std::string>{}(idString) ^ g_layoutGeneration.fetch_add(1);
    g_layout.loaded = true;
    CFRelease(source);
    }
}

void EnsureLayoutObserver() {
    static std::once_flag once;
    std::call_once(once, [] {
        [[NSDistributedNotificationCenter defaultCenter] addObserverForName:(__bridge NSString*)kTISNotifySelectedKeyboardInputSourceChanged
                                                                     object:nil
                                                                      queue:[NSOperationQueue mainQueue]
                                                                 usingBlock:^(NSNotification*) {
                                                                   LoadLayoutOnMainThread();
                                                                 }];
    });
}

void EnsureLayout() {
    {
        std::lock_guard<std::mutex> lock(g_layout.mutex);
        if (g_layout.loaded) return;
    }
    if ([NSThread isMainThread]) {
        LoadLayoutOnMainThread();
        EnsureLayoutObserver();
    } else {
        dispatch_async(dispatch_get_main_queue(), ^{
          LoadLayoutOnMainThread();
          EnsureLayoutObserver();
        });
    }
}

NSCursor* AsCursor(void* p) { return (__bridge NSCursor*)p; }
NSWindow* AsWindow(void* p) { return (__bridge NSWindow*)p; }

} // namespace

namespace MacOs {

void SetCoordinateScale(double scale) { g_coordinateScale.store(scale, std::memory_order_relaxed); }

void RefreshKeyboardLayout() {
    if (![NSThread isMainThread]) return;
    LoadLayoutOnMainThread();
    EnsureLayoutObserver();
}

bool IsMainThread() { return [NSThread isMainThread]; }

void RunOnMainThreadSync(const std::function<void()>& fn) {
    if ([NSThread isMainThread]) {
        @autoreleasepool {
            fn();
        }
        return;
    }
    dispatch_sync(dispatch_get_main_queue(), ^{
      fn();
    });
}

void RunOnMainThreadAsync(std::function<void()> fn) {
    auto* heap = new std::function<void()>(std::move(fn));
    dispatch_async(dispatch_get_main_queue(), ^{
      (*heap)();
      delete heap;
    });
}

// --- displays ---------------------------------------------------------------------

double PrimaryDisplayScale() {
    CGDirectDisplayID display = CGMainDisplayID();
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display);
    double scale = 1.0;
    if (mode) {
        const size_t pixelWidth = CGDisplayModeGetPixelWidth(mode);
        const size_t pointWidth = CGDisplayModeGetWidth(mode);
        if (pointWidth > 0) scale = static_cast<double>(pixelWidth) / static_cast<double>(pointWidth);
        CGDisplayModeRelease(mode);
    }
    return scale > 0.0 ? scale : 1.0;
}

std::vector<Display> Displays() {
    std::vector<Display> result;
    CGDirectDisplayID ids[32];
    uint32_t count = 0;
    if (CGGetActiveDisplayList(32, ids, &count) != kCGErrorSuccess) return result;
    const CGDirectDisplayID main = CGMainDisplayID();
    for (uint32_t i = 0; i < count; ++i) {
        Display d;
        d.id = ids[i];
        d.primary = ids[i] == main;
        d.frame = CGRectToPixels(CGDisplayBounds(ids[i]));
        d.workArea = d.frame;
        CGDisplayModeRef mode = CGDisplayCopyDisplayMode(ids[i]);
        if (mode) {
            const size_t pointWidth = CGDisplayModeGetWidth(mode);
            if (pointWidth > 0) d.scale = static_cast<double>(CGDisplayModeGetPixelWidth(mode)) / static_cast<double>(pointWidth);
            CGDisplayModeRelease(mode);
        }
        result.push_back(d);
    }
    // Work areas (menu bar / dock) need AppKit; only refine them on the main thread.
    if ([NSThread isMainThread]) @autoreleasepool {
        for (NSScreen* screen in [NSScreen screens]) {
            NSNumber* number = screen.deviceDescription[@"NSScreenNumber"];
            for (Display& d : result) {
                if (number && d.id == number.unsignedIntValue) {
                    d.workArea = CocoaRectToPixels(screen.visibleFrame);
                    d.name = ToStdString(screen.localizedName);
                }
            }
        }
    }
    std::stable_sort(result.begin(), result.end(), [](const Display& a, const Display& b) { return a.primary && !b.primary; });
    return result;
}

// --- windows ------------------------------------------------------------------------

bool WindowContentRect(void* nsWindow, PixelRect& out) {
    if (!nsWindow) return false;
    return OnMain([&]() -> bool {
        NSWindow* window = AsWindow(nsWindow);
        out = CocoaRectToPixels([window contentRectForFrameRect:window.frame]);
        return true;
    });
}

bool WindowFrameRect(void* nsWindow, PixelRect& out) {
    if (!nsWindow) return false;
    return OnMain([&]() -> bool {
        out = CocoaRectToPixels(AsWindow(nsWindow).frame);
        return true;
    });
}

double WindowBackingScale(void* nsWindow) {
    if (!nsWindow) return PrimaryDisplayScale();
    return OnMain([&]() -> double {
        NSWindow* window = AsWindow(nsWindow);
        if (!window.contentView.wantsBestResolutionOpenGLSurface) return 1.0;
        const double s = window.backingScaleFactor;
        return s > 0.0 ? s : 1.0;
    });
}

bool WindowIsKey(void* nsWindow) {
    if (!nsWindow) return false;
    return OnMain([&]() -> bool { return AsWindow(nsWindow).isKeyWindow && NSApp.isActive; });
}

bool WindowIsVisible(void* nsWindow) {
    if (!nsWindow) return false;
    return OnMain([&]() -> bool { return AsWindow(nsWindow).isVisible; });
}

bool WindowIsMiniaturized(void* nsWindow) {
    if (!nsWindow) return false;
    return OnMain([&]() -> bool { return AsWindow(nsWindow).isMiniaturized; });
}

bool WindowIsZoomed(void* nsWindow) {
    if (!nsWindow) return false;
    return OnMain([&]() -> bool { return AsWindow(nsWindow).isZoomed; });
}

bool WindowIsFullscreen(void* nsWindow) {
    if (!nsWindow) return false;
    return OnMain([&]() -> bool {
        NSWindow* window = AsWindow(nsWindow);
        if (window.styleMask & NSWindowStyleMaskFullScreen) return true;
        NSScreen* screen = window.screen;
        return screen && NSEqualRects(window.frame, screen.frame);
    });
}

bool ApplicationIsActive() {
    return OnMain([&]() -> bool { return NSApp.isActive; });
}

void WindowActivate(void* nsWindow) {
    if (!nsWindow) return;
    RunOnMainThreadAsync([nsWindow] {
        [NSApp activateIgnoringOtherApps:YES];
        [AsWindow(nsWindow) makeKeyAndOrderFront:nil];
    });
}

uint32_t WindowNumber(void* nsWindow) {
    if (!nsWindow) return 0;
    return OnMain([&]() -> uint32_t { return static_cast<uint32_t>(AsWindow(nsWindow).windowNumber); });
}

std::string WindowTitle(void* nsWindow) {
    if (!nsWindow) return {};
    return OnMain([&]() -> std::string { return ToStdString(AsWindow(nsWindow).title); });
}

void WindowSetContentRect(void* nsWindow, const PixelRect& rect) {
    if (!nsWindow) return;
    RunOnMainThreadSync([&] {
        NSWindow* window = AsWindow(nsWindow);
        NSRect content = PixelsToCocoaRect(rect);
        [window setFrame:[window frameRectForContentRect:content] display:YES];
    });
}

void WindowSetBorderless(void* nsWindow, bool borderless) {
    if (!nsWindow) return;
    RunOnMainThreadSync([&] {
        NSWindow* window = AsWindow(nsWindow);
        if (borderless) {
            NSScreen* screen = window.screen ?: [NSScreen mainScreen];
            window.styleMask = NSWindowStyleMaskBorderless;
            [window setFrame:screen.frame display:YES];
            window.level = NSMainMenuWindowLevel + 1;
            [NSApp setPresentationOptions:NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar];
        } else {
            window.level = NSNormalWindowLevel;
            [NSApp setPresentationOptions:NSApplicationPresentationDefault];
            window.styleMask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                               NSWindowStyleMaskResizable;
        }
    });
}

bool WindowIsBorderless(void* nsWindow) {
    if (!nsWindow) return false;
    return OnMain([&]() -> bool { return AsWindow(nsWindow).styleMask == NSWindowStyleMaskBorderless; });
}

void WindowMiniaturize(void* nsWindow) {
    if (!nsWindow) return;
    RunOnMainThreadAsync([nsWindow] { [AsWindow(nsWindow) miniaturize:nil]; });
}

void WindowRestore(void* nsWindow) {
    if (!nsWindow) return;
    RunOnMainThreadAsync([nsWindow] {
        NSWindow* window = AsWindow(nsWindow);
        if (window.isMiniaturized) [window deminiaturize:nil];
    });
}

// --- other windows -----------------------------------------------------------------------

static bool FillForeignWindow(NSDictionary* info, ForeignWindow& w) {
    NSNumber* number = info[(id)kCGWindowNumber];
    if (!number) return false;
    w.id = number.unsignedIntValue;
    w.pid = [info[(id)kCGWindowOwnerPID] intValue];
    w.layer = [info[(id)kCGWindowLayer] intValue];
    w.onScreen = [info[(id)kCGWindowIsOnscreen] boolValue];
    w.title = ToStdString(info[(id)kCGWindowName]);
    w.ownerName = ToStdString(info[(id)kCGWindowOwnerName]);
    CGRect bounds = CGRectZero;
    NSDictionary* boundsDict = info[(id)kCGWindowBounds];
    if (boundsDict) CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)boundsDict, &bounds);
    w.bounds = CGRectToPixels(bounds);
    char path[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(w.pid, path, sizeof(path)) > 0) w.ownerPath = path;
    return true;
}

std::vector<ForeignWindow> ListWindows(bool onScreenOnly) {
    @autoreleasepool {
    std::vector<ForeignWindow> result;
    const CGWindowListOption options =
        (onScreenOnly ? kCGWindowListOptionOnScreenOnly : kCGWindowListOptionAll) | kCGWindowListExcludeDesktopElements;
    CFArrayRef list = CGWindowListCopyWindowInfo(options, kCGNullWindowID);
    if (!list) return result;
    for (NSDictionary* info in (__bridge NSArray*)list) {
        ForeignWindow w;
        if (FillForeignWindow(info, w)) result.push_back(std::move(w));
    }
    CFRelease(list);
    return result;
    }
}

bool WindowInfo(uint32_t windowId, ForeignWindow& out) {
    @autoreleasepool {
    CFArrayRef ids = CFArrayCreate(nullptr, reinterpret_cast<const void**>(&windowId), 1, nullptr);
    CFArrayRef list = CGWindowListCreateDescriptionFromArray(ids);
    CFRelease(ids);
    bool found = false;
    if (list) {
        if (CFArrayGetCount(list) > 0) found = FillForeignWindow((__bridge NSDictionary*)CFArrayGetValueAtIndex(list, 0), out);
        CFRelease(list);
    }
    return found;
    }
}

// --- cursor ------------------------------------------------------------------------------

bool CursorPosition(int& x, int& y) {
    CGEventRef event = CGEventCreate(nullptr);
    if (!event) return false;
    const CGPoint p = CGEventGetLocation(event);
    CFRelease(event);
    const double s = CoordinateScale();
    x = static_cast<int>(std::lround(p.x * s));
    y = static_cast<int>(std::lround(p.y * s));
    return true;
}

void WarpCursor(int x, int y) {
    const double s = CoordinateScale();
    CGWarpMouseCursorPosition(CGPointMake(x / s, y / s));
}

void SetCursorAssociated(bool associated) { CGAssociateMouseAndMouseCursorPosition(associated ? true : false); }

void* CreateCursor(const uint8_t* rgba, int width, int height, int hotX, int hotY) {
    if (!rgba || width <= 0 || height <= 0) return nullptr;
    return OnMain([&]() -> void* {
        NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                                                                        pixelsWide:width
                                                                        pixelsHigh:height
                                                                     bitsPerSample:8
                                                                   samplesPerPixel:4
                                                                          hasAlpha:YES
                                                                          isPlanar:NO
                                                                    colorSpaceName:NSDeviceRGBColorSpace
                                                                      bitmapFormat:NSBitmapFormatAlphaNonpremultiplied
                                                                       bytesPerRow:width * 4
                                                                      bitsPerPixel:32];
        if (!rep) return nullptr;
        std::memcpy(rep.bitmapData, rgba, static_cast<size_t>(width) * height * 4);
        // Cursor files are authored in pixels; show them at that physical size.
        const double scale = CoordinateScale();
        NSImage* image = [[NSImage alloc] initWithSize:NSMakeSize(width / scale, height / scale)];
        [image addRepresentation:rep];
        NSCursor* cursor = [[NSCursor alloc] initWithImage:image hotSpot:NSMakePoint(hotX / scale, hotY / scale)];
        return (__bridge_retained void*)cursor;
    });
}

void* SystemCursor(SystemCursorKind kind) {
    return OnMain([&]() -> void* {
        NSCursor* cursor = nil;
        switch (kind) {
        case SystemCursorKind::Arrow: cursor = [NSCursor arrowCursor]; break;
        case SystemCursorKind::IBeam: cursor = [NSCursor IBeamCursor]; break;
        case SystemCursorKind::Wait: cursor = [NSCursor arrowCursor]; break;
        case SystemCursorKind::Cross: cursor = [NSCursor crosshairCursor]; break;
        case SystemCursorKind::Hand: cursor = [NSCursor pointingHandCursor]; break;
        case SystemCursorKind::ResizeEW: cursor = [NSCursor resizeLeftRightCursor]; break;
        case SystemCursorKind::ResizeNS: cursor = [NSCursor resizeUpDownCursor]; break;
        case SystemCursorKind::ResizeNWSE: cursor = [NSCursor crosshairCursor]; break;
        case SystemCursorKind::ResizeNESW: cursor = [NSCursor crosshairCursor]; break;
        case SystemCursorKind::ResizeAll: cursor = [NSCursor closedHandCursor]; break;
        case SystemCursorKind::NotAllowed: cursor = [NSCursor operationNotAllowedCursor]; break;
        }
        return cursor ? (__bridge_retained void*)cursor : nullptr;
    });
}

void ReleaseCursor(void* cursor) {
    if (cursor) CFRelease(cursor);
}

void SetCursor(void* cursor) {
    if (!cursor) return;
    if ([NSThread isMainThread]) {
        [AsCursor(cursor) set];
    } else {
        CFRetain(cursor);
        dispatch_async(dispatch_get_main_queue(), ^{
          [AsCursor(cursor) set];
          CFRelease(cursor);
        });
    }
}

void HideCursor() {
    if ([NSThread isMainThread]) [NSCursor hide];
    else dispatch_async(dispatch_get_main_queue(), ^{ [NSCursor hide]; });
}

void UnhideCursor() {
    if ([NSThread isMainThread]) [NSCursor unhide];
    else dispatch_async(dispatch_get_main_queue(), ^{ [NSCursor unhide]; });
}

bool CursorImage(void* cursor, std::vector<uint8_t>& rgba, int& width, int& height, int& hotX, int& hotY) {
    if (!cursor) return false;
    return OnMain([&]() -> bool {
        NSCursor* c = AsCursor(cursor);
        NSImage* image = c.image;
        if (!image) return false;
        const double scale = CoordinateScale();
        width = static_cast<int>(std::lround(image.size.width * scale));
        height = static_cast<int>(std::lround(image.size.height * scale));
        if (width <= 0 || height <= 0) return false;
        hotX = static_cast<int>(std::lround(c.hotSpot.x * scale));
        hotY = static_cast<int>(std::lround(c.hotSpot.y * scale));
        rgba.assign(static_cast<size_t>(width) * height * 4, 0);
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreate(rgba.data(), width, height, 8, width * 4, space,
                                                 static_cast<uint32_t>(kCGImageAlphaPremultipliedLast) | static_cast<uint32_t>(kCGBitmapByteOrder32Big));
        CGColorSpaceRelease(space);
        if (!ctx) return false;
        NSRect rect = NSMakeRect(0, 0, width, height);
        CGImageRef cg = [image CGImageForProposedRect:&rect context:nil hints:nil];
        if (cg) CGContextDrawImage(ctx, CGRectMake(0, 0, width, height), cg);
        CGContextRelease(ctx);
        // Un-premultiply.
        for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
            const uint8_t a = rgba[i + 3];
            if (a != 0 && a != 255) {
                rgba[i + 0] = static_cast<uint8_t>(std::min(255, rgba[i + 0] * 255 / a));
                rgba[i + 1] = static_cast<uint8_t>(std::min(255, rgba[i + 1] * 255 / a));
                rgba[i + 2] = static_cast<uint8_t>(std::min(255, rgba[i + 2] * 255 / a));
            }
        }
        return cg != nullptr;
    });
}

int SystemCursorSizePixels() { return static_cast<int>(std::lround(32.0 * CoordinateScale())); }

// --- keyboard -------------------------------------------------------------------------------

std::u16string TranslateKey(uint16_t keyCode, bool shift, bool option, bool capsLock, bool control) {
    EnsureLayout();
    std::lock_guard<std::mutex> lock(g_layout.mutex);
    if (!g_layout.layoutData) return {};
    const UCKeyboardLayout* layout = reinterpret_cast<const UCKeyboardLayout*>(CFDataGetBytePtr(g_layout.layoutData));
    UInt32 modifiers = 0;
    if (shift) modifiers |= shiftKey;
    if (option) modifiers |= optionKey;
    if (capsLock) modifiers |= alphaLock;
    if (control) modifiers |= controlKey;
    UInt32 deadKeyState = 0;
    UniChar chars[8];
    UniCharCount length = 0;
    const OSStatus status = UCKeyTranslate(layout, keyCode, kUCKeyActionDown, (modifiers >> 8) & 0xFF, g_layout.keyboardType,
                                           kUCKeyTranslateNoDeadKeysBit, &deadKeyState, 8, &length, chars);
    if (status != noErr || length == 0) return {};
    return std::u16string(reinterpret_cast<const char16_t*>(chars), length);
}

uint64_t KeyboardLayoutId() {
    EnsureLayout();
    std::lock_guard<std::mutex> lock(g_layout.mutex);
    return g_layout.id;
}

std::string KeyboardLayoutLanguage() {
    EnsureLayout();
    std::lock_guard<std::mutex> lock(g_layout.mutex);
    return g_layout.language.empty() ? std::string("en") : g_layout.language;
}

std::string KeyboardLayoutName() {
    EnsureLayout();
    std::lock_guard<std::mutex> lock(g_layout.mutex);
    return g_layout.name;
}

int PhysicalKeyboardType() {
    EnsureLayout();
    UInt32 type = 0;
    {
        std::lock_guard<std::mutex> lock(g_layout.mutex);
        type = g_layout.keyboardType;
    }
    switch (KBGetLayoutType(static_cast<int16_t>(type))) {
    case kKeyboardISO: return 1;
    case kKeyboardJIS: return 2;
    default: return 0;
    }
}

bool CapsLockOn() { return (CGEventSourceFlagsState(kCGEventSourceStateHIDSystemState) & kCGEventFlagMaskAlphaShift) != 0; }

int KeyRepeatInitialDelayMs() { return static_cast<int>(std::lround([NSEvent keyRepeatDelay] * 1000.0)); }

int KeyRepeatIntervalMs() { return static_cast<int>(std::lround([NSEvent keyRepeatInterval] * 1000.0)); }

// --- clipboard / shell / dialogs ------------------------------------------------------------

bool SetClipboardText(const std::string& utf8) {
    return OnMain([&]() -> bool {
        NSPasteboard* board = [NSPasteboard generalPasteboard];
        [board clearContents];
        return [board setString:ToNSString(utf8) forType:NSPasteboardTypeString];
    });
}

bool GetClipboardText(std::string& utf8) {
    return OnMain([&]() -> bool {
        NSString* s = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
        if (!s) return false;
        utf8 = ToStdString(s);
        return true;
    });
}

bool OpenUrl(const std::string& urlOrPath) {
    NSString* s = ToNSString(urlOrPath);
    NSURL* url = nil;
    if ([s hasPrefix:@"/"] || [s hasPrefix:@"~"]) {
        url = [NSURL fileURLWithPath:[s stringByExpandingTildeInPath]];
    } else {
        url = [NSURL URLWithString:s];
    }
    if (!url) return false;
    return OnMain([&]() -> bool { return [[NSWorkspace sharedWorkspace] openURL:url]; });
}

bool ChooseFile(const std::string& title, const std::string& initialDirectory, const std::vector<std::string>& extensions,
                std::string& outPath) {
    return OnMain([&]() -> bool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
        if (!title.empty()) panel.message = ToNSString(title);
        if (!initialDirectory.empty()) panel.directoryURL = [NSURL fileURLWithPath:ToNSString(initialDirectory)];
        if (!extensions.empty()) {
            NSMutableArray<UTType*>* types = [NSMutableArray array];
            for (const std::string& ext : extensions) {
                UTType* type = [UTType typeWithFilenameExtension:ToNSString(ext)];
                if (type) [types addObject:type];
            }
            if (types.count > 0) panel.allowedContentTypes = types;
        }
        NSWindow* key = NSApp.keyWindow;
        const NSModalResponse response = [panel runModal];
        [key makeKeyAndOrderFront:nil];
        if (response != NSModalResponseOK || panel.URLs.count == 0) return false;
        outPath = ToStdString(panel.URLs[0].path);
        return true;
    });
}

bool ChooseSaveFile(const std::string& title, const std::string& initialDirectory, const std::string& defaultName,
                    std::string& outPath) {
    return OnMain([&]() -> bool {
        NSSavePanel* panel = [NSSavePanel savePanel];
        if (!title.empty()) panel.message = ToNSString(title);
        if (!initialDirectory.empty()) panel.directoryURL = [NSURL fileURLWithPath:ToNSString(initialDirectory)];
        if (!defaultName.empty()) panel.nameFieldStringValue = ToNSString(defaultName);
        NSWindow* key = NSApp.keyWindow;
        const NSModalResponse response = [panel runModal];
        [key makeKeyAndOrderFront:nil];
        if (response != NSModalResponseOK || !panel.URL) return false;
        outPath = ToStdString(panel.URL.path);
        return true;
    });
}

int ShowAlert(const std::string& title, const std::string& text, const std::vector<std::string>& buttons, bool critical) {
    return OnMain([&]() -> int {
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = ToNSString(title);
        alert.informativeText = ToNSString(text);
        alert.alertStyle = critical ? NSAlertStyleCritical : NSAlertStyleInformational;
        for (const std::string& b : buttons) [alert addButtonWithTitle:ToNSString(b)];
        if (buttons.empty()) [alert addButtonWithTitle:@"OK"];
        const NSModalResponse response = [alert runModal];
        return static_cast<int>(response - NSAlertFirstButtonReturn);
    });
}

// --- paths ------------------------------------------------------------------------------------

static std::string SearchPath(NSSearchPathDirectory dir) {
    NSArray* paths = NSSearchPathForDirectoriesInDomains(dir, NSUserDomainMask, YES);
    return paths.count > 0 ? ToStdString(paths[0]) : std::string();
}

std::string HomeDirectory() { return ToStdString(NSHomeDirectory()); }

std::string TemporaryDirectory() {
    std::string tmp = ToStdString(NSTemporaryDirectory());
    if (tmp.empty()) tmp = "/tmp/";
    if (tmp.back() != '/') tmp.push_back('/');
    return tmp;
}

std::string DownloadsDirectory() { return SearchPath(NSDownloadsDirectory); }
std::string DocumentsDirectory() { return SearchPath(NSDocumentDirectory); }
std::string DesktopDirectory() { return SearchPath(NSDesktopDirectory); }
std::string PicturesDirectory() { return SearchPath(NSPicturesDirectory); }
std::string ApplicationSupportDirectory() { return SearchPath(NSApplicationSupportDirectory); }

std::vector<std::string> FontDirectories() {
    return { "/System/Library/Fonts", "/System/Library/Fonts/Supplemental", "/Library/Fonts", HomeDirectory() + "/Library/Fonts" };
}

// --- processes --------------------------------------------------------------------------------

bool ProcessStartTimeUs(int pid, uint64_t& outUs) {
    struct proc_bsdinfo info {};
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != static_cast<int>(sizeof(info))) return false;
    outUs = static_cast<uint64_t>(info.pbi_start_tvsec) * 1000000ull + static_cast<uint64_t>(info.pbi_start_tvusec);
    return true;
}

bool ProcessAlive(int pid) {
    if (pid <= 0) return false;
    return kill(pid, 0) == 0 || errno == EPERM;
}

std::string ProcessPath(int pid) {
    char path[PROC_PIDPATHINFO_MAXSIZE];
    return proc_pidpath(pid, path, sizeof(path)) > 0 ? std::string(path) : std::string();
}

std::vector<std::string> CommandLineArguments() {
    std::vector<std::string> args;
    int argc = *_NSGetArgc();
    char** argv = *_NSGetArgv();
    for (int i = 0; i < argc; ++i) args.emplace_back(argv[i] ? argv[i] : "");
    return args;
}

// --- HTTP -----------------------------------------------------------------------------------

bool HttpRequest(const std::string& method, const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
                 const std::vector<uint8_t>& body, int timeoutMs, HttpResponse& out) {
    @autoreleasepool {
        NSURL* nsUrl = [NSURL URLWithString:ToNSString(url)];
        if (!nsUrl) {
            out.error = "invalid url";
            return false;
        }
        NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:nsUrl];
        request.HTTPMethod = ToNSString(method);
        request.timeoutInterval = timeoutMs > 0 ? timeoutMs / 1000.0 : 30.0;
        for (const auto& h : headers) [request setValue:ToNSString(h.second) forHTTPHeaderField:ToNSString(h.first)];
        if (!body.empty()) request.HTTPBody = [NSData dataWithBytes:body.data() length:body.size()];

        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        __block NSData* data = nil;
        __block NSHTTPURLResponse* response = nil;
        __block NSError* error = nil;
        NSURLSessionDataTask* task =
            [[NSURLSession sharedSession] dataTaskWithRequest:request
                                            completionHandler:^(NSData* d, NSURLResponse* r, NSError* e) {
                                              data = d;
                                              response = [r isKindOfClass:[NSHTTPURLResponse class]] ? (NSHTTPURLResponse*)r : nil;
                                              error = e;
                                              dispatch_semaphore_signal(done);
                                            }];
        [task resume];
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);

        if (error || !response) {
            out.error = error ? ToStdString(error.localizedDescription) : std::string("no response");
            return false;
        }
        out.status = static_cast<int>(response.statusCode);
        [response.allHeaderFields enumerateKeysAndObjectsUsingBlock:^(id key, id value, BOOL*) {
          out.headers.emplace_back(ToStdString([key description]), ToStdString([value description]));
        }];
        if (data.length > 0) {
            const auto* bytes = static_cast<const uint8_t*>(data.bytes);
            out.body.assign(bytes, bytes + data.length);
        }
        return true;
    }
}

} // namespace MacOs
