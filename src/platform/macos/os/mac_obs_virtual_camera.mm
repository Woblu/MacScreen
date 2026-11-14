#import <CoreMedia/CoreMedia.h>
#import <CoreMediaIO/CMIOHardware.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include "platform/macos/os/mac_obs_virtual_camera.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace MacObsVirtualCamera {
namespace {

// Layout of the queue written by src/features/virtual_camera.cpp (OBS's
// shared-memory-queue.h): this header, then three slots, each a 32-byte frame
// header followed by an NV12 frame.
struct QueueHeader {
    volatile uint32_t write_idx;
    volatile uint32_t read_idx;
    volatile uint32_t state;
    uint32_t offsets[3];
    uint32_t type;
    uint32_t cx;
    uint32_t cy;
    uint64_t interval;
    uint32_t reserved[8];
};

constexpr uint32_t kQueueStateReady = 2;
constexpr size_t kFrameHeaderSize = 32;

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

CMIOObjectID FindObsCameraDevice() {
    CMIOObjectPropertyAddress address{ kCMIOHardwarePropertyDevices, kCMIOObjectPropertyScopeGlobal, kCMIOObjectPropertyElementMain };
    UInt32 size = 0;
    if (CMIOObjectGetPropertyDataSize(kCMIOObjectSystemObject, &address, 0, nullptr, &size) != kCMIOHardwareNoError || size == 0) {
        return kCMIOObjectUnknown;
    }
    std::vector<CMIOObjectID> devices(size / sizeof(CMIOObjectID));
    UInt32 used = 0;
    if (CMIOObjectGetPropertyData(kCMIOObjectSystemObject, &address, 0, nullptr, size, &used, devices.data()) != kCMIOHardwareNoError) {
        return kCMIOObjectUnknown;
    }
    devices.resize(used / sizeof(CMIOObjectID));

    for (CMIOObjectID device : devices) {
        CMIOObjectPropertyAddress uidAddress{ kCMIODevicePropertyDeviceUID, kCMIOObjectPropertyScopeGlobal, kCMIOObjectPropertyElementMain };
        CFStringRef uid = nullptr;
        UInt32 uidSize = 0;
        if (CMIOObjectGetPropertyData(device, &uidAddress, 0, nullptr, sizeof(uid), &uidSize, &uid) != kCMIOHardwareNoError || !uid) {
            continue;
        }
        // VIRTUALCAM_DEVICE_UUID in OBS Studio's macOS build preset.
        const bool isObs = CFStringCompare(uid, CFSTR("7626645E-4425-469E-9D8B-97E0FA59AC75"), kCFCompareCaseInsensitive) == kCFCompareEqualTo;
        CFRelease(uid);
        if (isObs) return device;
    }
    return kCMIOObjectUnknown;
}

CMIOStreamID FindSinkStream(CMIOObjectID device) {
    CMIOObjectPropertyAddress address{ kCMIODevicePropertyStreams, kCMIOObjectPropertyScopeGlobal, kCMIOObjectPropertyElementMain };
    UInt32 size = 0;
    if (CMIOObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) != kCMIOHardwareNoError || size < 2 * sizeof(CMIOStreamID)) {
        return kCMIOObjectUnknown;
    }
    std::vector<CMIOStreamID> streams(size / sizeof(CMIOStreamID));
    UInt32 used = 0;
    if (CMIOObjectGetPropertyData(device, &address, 0, nullptr, size, &used, streams.data()) != kCMIOHardwareNoError ||
        used < 2 * sizeof(CMIOStreamID)) {
        return kCMIOObjectUnknown;
    }
    // The extension adds its source stream first and its sink stream second.
    return streams[1];
}

void QueueAltered(CMIOStreamID, void*, void*) {}

// The extension's sink stream, opened the way OBS's macOS plugin opens it.
class Sink {
public:
    ~Sink() { Close(); }

    bool IsOpen() const { return queue_ != nullptr; }

    bool Open() {
        device_ = FindObsCameraDevice();
        if (device_ == kCMIOObjectUnknown) return false;
        stream_ = FindSinkStream(device_);
        if (stream_ == kCMIOObjectUnknown) return false;
        if (CMIOStreamCopyBufferQueue(stream_, &QueueAltered, nullptr, &queue_) != kCMIOHardwareNoError || !queue_) {
            queue_ = nullptr;
            return false;
        }
        if (CMIODeviceStartStream(device_, stream_) != kCMIOHardwareNoError) {
            CFRelease(queue_);
            queue_ = nullptr;
            return false;
        }
        return true;
    }

    void Close() {
        if (queue_) {
            CMIODeviceStopStream(device_, stream_);
            CFRelease(queue_);
            queue_ = nullptr;
        }
        if (pool_) {
            CVPixelBufferPoolRelease(pool_);
            pool_ = nullptr;
        }
        if (format_) {
            CFRelease(format_);
            format_ = nullptr;
        }
        width_ = 0;
        height_ = 0;
    }

    void Send(const uint8_t* nv12, uint32_t width, uint32_t height) {
        if (!pool_ || width != width_ || height != height_) {
            if (pool_) CVPixelBufferPoolRelease(pool_);
            if (format_) CFRelease(format_);
            pool_ = nullptr;
            format_ = nullptr;
            @autoreleasepool {
                NSDictionary* attributes = @{
                    (__bridge NSString*)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
                    (__bridge NSString*)kCVPixelBufferWidthKey : @(width),
                    (__bridge NSString*)kCVPixelBufferHeightKey : @(height),
                    (__bridge NSString*)kCVPixelBufferIOSurfacePropertiesKey : @{},
                };
                if (CVPixelBufferPoolCreate(kCFAllocatorDefault, nullptr, (__bridge CFDictionaryRef)attributes, &pool_) != kCVReturnSuccess) {
                    pool_ = nullptr;
                    return;
                }
            }
            width_ = width;
            height_ = height;
        }

        CVPixelBufferRef pixelBuffer = nullptr;
        if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool_, &pixelBuffer) != kCVReturnSuccess || !pixelBuffer) return;

        // Same rule as the NV12 conversion in render.cpp and virtual_camera.cpp.
        const bool bt709 = width >= 1280 || height > 576;
        CVBufferSetAttachment(pixelBuffer, kCVImageBufferYCbCrMatrixKey,
                              bt709 ? kCVImageBufferYCbCrMatrix_ITU_R_709_2 : kCVImageBufferYCbCrMatrix_ITU_R_601_4,
                              kCVAttachmentMode_ShouldPropagate);

        CVPixelBufferLockBaseAddress(pixelBuffer, 0);
        auto* yPlane = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0));
        auto* uvPlane = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1));
        const size_t yStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0);
        const size_t uvStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1);
        const uint8_t* uvSource = nv12 + static_cast<size_t>(width) * height;
        for (uint32_t row = 0; row < height; ++row) {
            std::memcpy(yPlane + row * yStride, nv12 + static_cast<size_t>(row) * width, width);
        }
        for (uint32_t row = 0; row < height / 2; ++row) {
            std::memcpy(uvPlane + row * uvStride, uvSource + static_cast<size_t>(row) * width, width);
        }
        CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);

        if (!format_ && CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, pixelBuffer, &format_) != noErr) {
            format_ = nullptr;
            CVPixelBufferRelease(pixelBuffer);
            return;
        }

        CMSampleTimingInfo timing{ kCMTimeInvalid, CMClockGetTime(CMClockGetHostTimeClock()), kCMTimeInvalid };
        CMSampleBufferRef sample = nullptr;
        const OSStatus status =
            CMSampleBufferCreateForImageBuffer(kCFAllocatorDefault, pixelBuffer, true, nullptr, nullptr, format_, &timing, &sample);
        CVPixelBufferRelease(pixelBuffer);
        if (status != noErr || !sample) return;

        // The queue takes the reference; when it is full the frame is dropped.
        if (CMSimpleQueueEnqueue(queue_, sample) != noErr) CFRelease(sample);
    }

private:
    CMIOObjectID device_ = kCMIOObjectUnknown;
    CMIOStreamID stream_ = kCMIOObjectUnknown;
    CMSimpleQueueRef queue_ = nullptr;
    CVPixelBufferPoolRef pool_ = nullptr;
    CMVideoFormatDescriptionRef format_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

struct Session {
    const uint8_t* base = nullptr;
    size_t size = 0;
    std::atomic<bool> stop{ false };
    std::thread thread;
};

std::mutex g_sessionMutex;
std::unique_ptr<Session> g_session;

std::atomic<bool> g_available{ false };
std::atomic<bool> g_checkInFlight{ false };
std::atomic<int64_t> g_lastCheckMs{ 0 };

// Reads the queue the way OBS's Windows camera driver does: a new read_idx
// while the queue is ready means a new frame in slot read_idx % 3.
void Run(Session* session) {
    Sink sink;
    const auto* header = reinterpret_cast<const QueueHeader*>(session->base);
    uint32_t lastReadIndex = UINT32_MAX;
    int64_t nextOpenAttemptMs = 0;

    while (!session->stop.load(std::memory_order_acquire)) {
        const uint32_t state = header->state;
        const uint32_t readIndex = header->read_idx;
        std::atomic_thread_fence(std::memory_order_acquire);

        if (state == kQueueStateReady && readIndex != lastReadIndex) {
            if (!sink.IsOpen() && NowMs() >= nextOpenAttemptMs && !sink.Open()) {
                nextOpenAttemptMs = NowMs() + 2000;
            }
            if (sink.IsOpen()) {
                lastReadIndex = readIndex;
                const uint32_t width = header->cx;
                const uint32_t height = header->cy;
                const size_t offset = header->offsets[readIndex % 3];
                const size_t frameBytes = static_cast<size_t>(width) * height * 3 / 2;
                if (width >= 2 && height >= 2 && offset + kFrameHeaderSize + frameBytes <= session->size) {
                    sink.Send(session->base + offset + kFrameHeaderSize, width, height);
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

} // namespace

bool IsAvailable() {
    const int64_t now = NowMs();
    const int64_t last = g_lastCheckMs.load(std::memory_order_acquire);
    if ((last == 0 || now - last >= 2000) && !g_checkInFlight.exchange(true)) {
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
          g_available.store(FindObsCameraDevice() != kCMIOObjectUnknown, std::memory_order_release);
          g_lastCheckMs.store(NowMs(), std::memory_order_release);
          g_checkInFlight.store(false);
        });
    }
    return g_available.load(std::memory_order_acquire);
}

void Attach(void* base, size_t size) {
    std::lock_guard<std::mutex> lock(g_sessionMutex);
    if (g_session || !base || size < sizeof(QueueHeader)) return;
    auto session = std::make_unique<Session>();
    session->base = static_cast<const uint8_t*>(base);
    session->size = size;
    session->thread = std::thread(Run, session.get());
    g_session = std::move(session);
}

void Detach(void* base) {
    std::unique_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(g_sessionMutex);
        if (!g_session || static_cast<const void*>(g_session->base) != base) return;
        session = std::move(g_session);
    }
    session->stop.store(true, std::memory_order_release);
    session->thread.join();
}

} // namespace MacObsVirtualCamera
