#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include <objc/message.h>

#include "platform/macos/os/mac_browser_view.h"

#include <algorithm>
#include <cstdint>

@interface TSBrowserOverlayHost : NSObject <WKNavigationDelegate>
@end

@implementation TSBrowserOverlayHost {
  @public
    NSWindow* _window;
    WKWebView* _webView;
    NSURL* _lastURL;
    bool _lastNavigationFailed;
    MacBrowserView::Callbacks _callbacks;
}

- (void)webView:(WKWebView*)webView didCommitNavigation:(WKNavigation*)navigation {
    _lastNavigationFailed = false;
    if (_callbacks.contentLoading) _callbacks.contentLoading();
}

- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)navigation {
    if (_callbacks.navigationCompleted) _callbacks.navigationCompleted(true, 0);
}

- (void)webView:(WKWebView*)webView didFailNavigation:(WKNavigation*)navigation withError:(NSError*)error {
    if (_callbacks.navigationCompleted) _callbacks.navigationCompleted(false, static_cast<long>(error.code));
}

- (void)webView:(WKWebView*)webView didFailProvisionalNavigation:(WKNavigation*)navigation withError:(NSError*)error {
    _lastNavigationFailed = true;
    if (_callbacks.navigationCompleted) _callbacks.navigationCompleted(false, static_cast<long>(error.code));
}

- (void)webViewWebContentProcessDidTerminate:(WKWebView*)webView {
    if (_callbacks.processFailed) _callbacks.processFailed();
}

@end

namespace MacBrowserView {
namespace {

NSMutableDictionary<NSString*, TSBrowserOverlayHost*>* Hosts() {
    static NSMutableDictionary<NSString*, TSBrowserOverlayHost*>* hosts = [[NSMutableDictionary alloc] init];
    return hosts;
}

NSString* ToNSString(const std::string& value) {
    NSString* result = [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
    if (!result) result = [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSISOLatin1StringEncoding];
    return result ?: @"";
}

TSBrowserOverlayHost* HostFor(const std::string& id) { return Hosts()[ToNSString(id)]; }

void OnMain(dispatch_block_t block) { dispatch_async(dispatch_get_main_queue(), block); }

// WebKit private setters, present since macOS 10.13/10.14. Skipped when missing.
bool CallBoolSetter(id target, NSString* selectorName, BOOL value) {
    SEL selector = NSSelectorFromString(selectorName);
    if (![target respondsToSelector:selector]) return false;
    ((void (*)(id, SEL, BOOL))objc_msgSend)(target, selector, value);
    return true;
}

bool CallUIntegerSetter(id target, NSString* selectorName, NSUInteger value) {
    SEL selector = NSSelectorFromString(selectorName);
    if (![target respondsToSelector:selector]) return false;
    ((void (*)(id, SEL, NSUInteger))objc_msgSend)(target, selector, value);
    return true;
}

NSScreen* HostScreen() {
    NSArray<NSScreen*>* screens = NSScreen.screens;
    return screens.count > 0 ? screens.firstObject : nil;
}

void ApplySize(TSBrowserOverlayHost* host, int width, int height) {
    NSScreen* screen = HostScreen();
    const CGFloat scale = screen && screen.backingScaleFactor > 0 ? screen.backingScaleFactor : 1.0;
    const NSPoint origin = screen ? screen.frame.origin : NSZeroPoint;
    const NSRect frame = NSMakeRect(origin.x, origin.y, (std::max)(1, width) / scale, (std::max)(1, height) / scale);
    [host->_window setFrame:frame display:NO];
}

void DestroyHost(TSBrowserOverlayHost* host) {
    host->_callbacks = {};
    host->_webView.navigationDelegate = nil;
    [host->_webView stopLoading];
    [host->_window orderOut:nil];
    host->_window.contentView = nil;
    [host->_window close];
    host->_webView = nil;
    host->_window = nil;
}

} // namespace

void Create(const std::string& id, int width, int height, Callbacks callbacks, std::function<void(bool created)> done) {
    OnMain(^{
      @autoreleasepool {
          NSString* key = ToNSString(id);
          if (TSBrowserOverlayHost* existing = Hosts()[key]) {
              DestroyHost(existing);
              [Hosts() removeObjectForKey:key];
          }

          TSBrowserOverlayHost* host = [[TSBrowserOverlayHost alloc] init];
          host->_callbacks = callbacks;

          // The host window plays the part of the offscreen WebView2 host
          // window: never shown, never activated, ignored by Exposé and capture.
          NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1, 1)
                                                         styleMask:NSWindowStyleMaskBorderless
                                                           backing:NSBackingStoreBuffered
                                                             defer:NO];
          window.releasedWhenClosed = NO;
          window.opaque = NO;
          window.backgroundColor = NSColor.clearColor;
          window.alphaValue = 0.0;
          window.ignoresMouseEvents = YES;
          window.hasShadow = NO;
          window.excludedFromWindowsMenu = YES;
          window.sharingType = NSWindowSharingNone;
          window.animationBehavior = NSWindowAnimationBehaviorNone;
          window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorTransient |
                                      NSWindowCollectionBehaviorIgnoresCycle | NSWindowCollectionBehaviorFullScreenAuxiliary;

          WKWebViewConfiguration* configuration = [[WKWebViewConfiguration alloc] init];
          configuration.mediaTypesRequiringUserActionForPlayback = WKAudiovisualMediaTypeNone;

          WKWebView* webView = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, 1, 1) configuration:configuration];
          webView.navigationDelegate = host;
          webView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
          // WebKit suspends rendering for pages whose window is occluded, which
          // the host window always is.
          CallBoolSetter(webView, @"_setWindowOcclusionDetectionEnabled:", NO);

          window.contentView = webView;
          host->_window = window;
          host->_webView = webView;
          ApplySize(host, width, height);
          [window orderBack:nil];

          Hosts()[key] = host;
          if (done) done(true);
      }
    });
}

void Destroy(const std::string& id) {
    OnMain(^{
      @autoreleasepool {
          NSString* key = ToNSString(id);
          if (TSBrowserOverlayHost* host = Hosts()[key]) {
              DestroyHost(host);
              [Hosts() removeObjectForKey:key];
          }
      }
    });
}

void SetSize(const std::string& id, int width, int height) {
    OnMain(^{
      if (TSBrowserOverlayHost* host = HostFor(id)) ApplySize(host, width, height);
    });
}

void SetTransparentBackground(const std::string& id, bool transparent) {
    OnMain(^{
      if (TSBrowserOverlayHost* host = HostFor(id)) CallBoolSetter(host->_webView, @"_setDrawsBackground:", transparent ? NO : YES);
    });
}

void SetMuted(const std::string& id, bool muted) {
    OnMain(^{
      // _WKMediaAudioMuted
      if (TSBrowserOverlayHost* host = HostFor(id)) CallUIntegerSetter(host->_webView, @"_setPageMuted:", muted ? 1u : 0u);
    });
}

void Navigate(const std::string& id, const std::string& url) {
    OnMain(^{
      @autoreleasepool {
          TSBrowserOverlayHost* host = HostFor(id);
          if (!host) return;
          NSURL* target = [NSURL URLWithString:ToNSString(url)];
          if (!target) return;
          host->_lastURL = target;
          host->_lastNavigationFailed = false;
          if (target.isFileURL) {
              [host->_webView loadFileURL:target allowingReadAccessToURL:target.URLByDeletingLastPathComponent];
          } else {
              [host->_webView loadRequest:[NSURLRequest requestWithURL:target]];
          }
      }
    });
}

void Reload(const std::string& id) {
    OnMain(^{
      @autoreleasepool {
          TSBrowserOverlayHost* host = HostFor(id);
          if (!host) return;
          // Chromium commits an error page for a failed load, so its Reload
          // retries the failed address. WebKit keeps the previous page instead.
          if (host->_lastURL && (host->_lastNavigationFailed || !host->_webView.URL)) {
              host->_lastNavigationFailed = false;
              if (host->_lastURL.isFileURL) {
                  [host->_webView loadFileURL:host->_lastURL allowingReadAccessToURL:host->_lastURL.URLByDeletingLastPathComponent];
              } else {
                  [host->_webView loadRequest:[NSURLRequest requestWithURL:host->_lastURL]];
              }
              return;
          }
          [host->_webView reload];
      }
    });
}

void ExecuteScript(const std::string& id, const std::string& script) {
    OnMain(^{
      @autoreleasepool {
          if (TSBrowserOverlayHost* host = HostFor(id)) [host->_webView evaluateJavaScript:ToNSString(script) completionHandler:nil];
      }
    });
}

void Capture(const std::string& id, std::function<void(Snapshot snapshot)> done) {
    OnMain(^{
      @autoreleasepool {
          TSBrowserOverlayHost* host = HostFor(id);
          if (!host) {
              if (done) done(nullptr);
              return;
          }

          WKSnapshotConfiguration* configuration = [[WKSnapshotConfiguration alloc] init];
          configuration.afterScreenUpdates = NO;
          [host->_webView takeSnapshotWithConfiguration:configuration
                                      completionHandler:^(NSImage* image, NSError* error) {
                                        (void)error;
                                        Snapshot snapshot;
                                        CGImageRef cgImage = image ? [image CGImageForProposedRect:nullptr context:nil hints:nil] : nullptr;
                                        if (cgImage) {
                                            CGImageRetain(cgImage);
                                            snapshot = Snapshot(cgImage, [](CGImageRef retained) { CGImageRelease(retained); });
                                        }
                                        if (done) done(std::move(snapshot));
                                      }];
      }
    });
}

bool DecodeSnapshot(const Snapshot& snapshot, std::vector<unsigned char>& rgba, int& width, int& height) {
    CGImageRef image = static_cast<CGImageRef>(const_cast<void*>(snapshot.get()));
    if (!image) return false;

    const size_t imageWidth = CGImageGetWidth(image);
    const size_t imageHeight = CGImageGetHeight(image);
    if (imageWidth == 0 || imageHeight == 0 || imageWidth > 16384 || imageHeight > 16384) return false;

    rgba.assign(imageWidth * imageHeight * 4, 0);
    CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context =
        CGBitmapContextCreate(rgba.data(), imageWidth, imageHeight, 8, imageWidth * 4, colorSpace,
                              static_cast<uint32_t>(kCGImageAlphaPremultipliedLast) | static_cast<uint32_t>(kCGBitmapByteOrder32Big));
    CGColorSpaceRelease(colorSpace);
    if (!context) return false;

    CGContextSetBlendMode(context, kCGBlendModeCopy);
    CGContextDrawImage(context, CGRectMake(0, 0, imageWidth, imageHeight), image);
    CGContextRelease(context);

    // Bitmap contexts are premultiplied; WebView2's PNG captures are not.
    for (size_t offset = 0; offset < rgba.size(); offset += 4) {
        const unsigned alpha = rgba[offset + 3];
        if (alpha == 0 || alpha == 255) continue;
        for (size_t channel = 0; channel < 3; ++channel) {
            const unsigned value = (rgba[offset + channel] * 255u + alpha / 2) / alpha;
            rgba[offset + channel] = static_cast<unsigned char>((std::min)(value, 255u));
        }
    }

    width = static_cast<int>(imageWidth);
    height = static_cast<int>(imageHeight);
    return true;
}

} // namespace MacBrowserView
