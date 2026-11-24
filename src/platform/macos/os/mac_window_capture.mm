#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include "platform/macos/os/mac_window_capture.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace {

uint64_t NowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Frame {
    std::vector<uint8_t> pixels; // BGRA
    int width = 0;
    int height = 0;
    size_t stride = 0;
};

struct Session;

} // namespace

@interface TSWindowCaptureOutput : NSObject <SCStreamOutput, SCStreamDelegate>
@property(nonatomic, assign) Session* session;
@end

namespace {

struct Session {
    uint32_t windowId = 0;
    SCStream* stream = nil;
    TSWindowCaptureOutput* output = nil;
    dispatch_queue_t queue = nullptr;
    std::mutex frameMutex;
    Frame frame;
    std::atomic<bool> haveFrame{ false };
    bool starting = false;
    bool running = false;
    uint64_t lastRequestMs = 0;
    uint64_t retryAfterMs = 0;
    int configuredWidth = 0;
    int configuredHeight = 0;
};

std::mutex g_sessionsMutex;
std::map<uint32_t, std::shared_ptr<Session>> g_sessions;

void StopSession(const std::shared_ptr<Session>& session) {
    SCStream* stream = session->stream;
    session->stream = nil;
    session->running = false;
    if (stream) [stream stopCaptureWithCompletionHandler:^(NSError*) {}];
}

SCStreamConfiguration* MakeConfiguration(int width, int height) {
    SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
    config.width = static_cast<size_t>(std::max(1, width));
    config.height = static_cast<size_t>(std::max(1, height));
    config.pixelFormat = kCVPixelFormatType_32BGRA;
    config.showsCursor = NO;
    config.queueDepth = 3;
    config.minimumFrameInterval = CMTimeMake(1, 60);
    return config;
}

void StartSession(const std::shared_ptr<Session>& session, int width, int height) {
    session->starting = true;
    session->configuredWidth = width;
    session->configuredHeight = height;
    std::weak_ptr<Session> weak = session;
    const uint32_t windowId = session->windowId;
    [SCShareableContent getShareableContentExcludingDesktopWindows:NO
                                               onScreenWindowsOnly:NO
                                                 completionHandler:^(SCShareableContent* content, NSError* error) {
                                                   std::shared_ptr<Session> s = weak.lock();
                                                   if (!s) return;
                                                   std::lock_guard<std::mutex> lock(g_sessionsMutex);
                                                   s->starting = false;
                                                   SCWindow* target = nil;
                                                   for (SCWindow* w in content.windows) {
                                                       if (w.windowID == windowId) {
                                                           target = w;
                                                           break;
                                                       }
                                                   }
                                                   if (error || !target) {
                                                       // Usually missing Screen Recording permission; try again later.
                                                       s->retryAfterMs = NowMs() + 5000;
                                                       return;
                                                   }
                                                   SCContentFilter* filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:target];
                                                   SCStream* stream = [[SCStream alloc] initWithFilter:filter
                                                                                         configuration:MakeConfiguration(width, height)
                                                                                              delegate:s->output];
                                                   NSError* addError = nil;
                                                   if (![stream addStreamOutput:s->output
                                                                           type:SCStreamOutputTypeScreen
                                                             sampleHandlerQueue:s->queue
                                                                          error:&addError]) {
                                                       s->retryAfterMs = NowMs() + 5000;
                                                       return;
                                                   }
                                                   s->stream = stream;
                                                   s->running = true;
                                                   [stream startCaptureWithCompletionHandler:^(NSError* startError) {
                                                     if (!startError) return;
                                                     std::lock_guard<std::mutex> innerLock(g_sessionsMutex);
                                                     if (std::shared_ptr<Session> again = weak.lock()) {
                                                         again->running = false;
                                                         again->stream = nil;
                                                         again->retryAfterMs = NowMs() + 5000;
                                                     }
                                                   }];
                                                 }];
}

// Idle sessions stop capturing but stay allocated: stream callbacks may still
// be running on their queue and refer to the session.
void StopIdleSessionsLocked(uint64_t now) {
    for (auto& [id, session] : g_sessions) {
        if ((session->running || session->haveFrame) && now - session->lastRequestMs > 5000) {
            StopSession(session);
            std::lock_guard<std::mutex> frameLock(session->frameMutex);
            session->haveFrame = false;
            session->frame = Frame{};
        }
    }
}

void Scale(const Frame& src, int width, int height, uint8_t* dst, size_t dstStride) {
    for (int y = 0; y < height; ++y) {
        const int sy = std::min(src.height - 1, static_cast<int>((static_cast<int64_t>(y) * src.height) / height));
        const uint8_t* srcRow = src.pixels.data() + static_cast<size_t>(sy) * src.stride;
        uint8_t* dstRow = dst + static_cast<size_t>(y) * dstStride;
        if (src.width == width) {
            std::memcpy(dstRow, srcRow, static_cast<size_t>(width) * 4);
            continue;
        }
        for (int x = 0; x < width; ++x) {
            const int sx = std::min(src.width - 1, static_cast<int>((static_cast<int64_t>(x) * src.width) / width));
            std::memcpy(dstRow + x * 4, srcRow + sx * 4, 4);
        }
    }
}

} // namespace

@implementation TSWindowCaptureOutput

- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type {
    if (type != SCStreamOutputTypeScreen || !self.session) return;
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
    if (attachments && CFArrayGetCount(attachments) > 0) {
        NSDictionary* info = (__bridge NSDictionary*)CFArrayGetValueAtIndex(attachments, 0);
        NSNumber* status = info[SCStreamFrameInfoStatus];
        if (status && status.integerValue != SCFrameStatusComplete) return;
    }
    CVImageBufferRef image = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!image) return;
    CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    const size_t width = CVPixelBufferGetWidth(image);
    const size_t height = CVPixelBufferGetHeight(image);
    const size_t stride = CVPixelBufferGetBytesPerRow(image);
    const uint8_t* base = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(image));
    if (base && width > 0 && height > 0) {
        Session* s = self.session;
        std::lock_guard<std::mutex> lock(s->frameMutex);
        s->frame.width = static_cast<int>(width);
        s->frame.height = static_cast<int>(height);
        s->frame.stride = width * 4;
        s->frame.pixels.resize(s->frame.stride * height);
        for (size_t y = 0; y < height; ++y) {
            std::memcpy(s->frame.pixels.data() + y * s->frame.stride, base + y * stride, s->frame.stride);
        }
        s->haveFrame = true;
    }
    CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
    std::lock_guard<std::mutex> lock(g_sessionsMutex);
    if (Session* s = self.session) {
        s->running = false;
        s->stream = nil;
        s->retryAfterMs = NowMs() + 2000;
    }
}

@end

namespace MacWindowCapture {

namespace {

// Also stops idle sessions after the last window overlay is removed, when
// CopyFrame is no longer called.
void EnsureIdleTimerLocked() {
    static dispatch_source_t timer = nil;
    if (timer) return;
    timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), 2 * NSEC_PER_SEC, NSEC_PER_SEC / 2);
    dispatch_source_set_event_handler(timer, ^{
      std::lock_guard<std::mutex> lock(g_sessionsMutex);
      StopIdleSessionsLocked(NowMs());
    });
    dispatch_resume(timer);
}

} // namespace

bool CopyFrame(uint32_t windowId, int width, int height, uint8_t* bgra, size_t stride) {
    if (windowId == 0 || width <= 0 || height <= 0 || !bgra) return false;
    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(g_sessionsMutex);
        const uint64_t now = NowMs();
        StopIdleSessionsLocked(now);
        EnsureIdleTimerLocked();
        auto it = g_sessions.find(windowId);
        if (it == g_sessions.end()) {
            session = std::make_shared<Session>();
            session->windowId = windowId;
            session->output = [[TSWindowCaptureOutput alloc] init];
            session->output.session = session.get();
            session->queue = dispatch_queue_create("toolscreen.window-capture", DISPATCH_QUEUE_SERIAL);
            g_sessions[windowId] = session;
        } else {
            session = it->second;
        }
        session->lastRequestMs = now;
        if (!session->running && !session->starting && now >= session->retryAfterMs) {
            StartSession(session, width, height);
        } else if (session->running && session->stream &&
                   (session->configuredWidth != width || session->configuredHeight != height)) {
            session->configuredWidth = width;
            session->configuredHeight = height;
            [session->stream updateConfiguration:MakeConfiguration(width, height) completionHandler:^(NSError*) {}];
        }
    }

    std::lock_guard<std::mutex> lock(session->frameMutex);
    if (!session->haveFrame || session->frame.width <= 0 || session->frame.height <= 0) return false;
    Scale(session->frame, width, height, bgra, stride);
    return true;
}

} // namespace MacWindowCapture
