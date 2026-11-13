// The Windows build feeds the OBS Virtual Camera through its DirectShow
// shared-memory queue. macOS has no such queue (OBS uses a camera system
// extension there), so the virtual camera reports its driver as missing.

#include "features/virtual_camera.h"

std::atomic<bool> g_virtualCameraActive{ false };

bool StartVirtualCamera(uint32_t, uint32_t) { return false; }
void StopVirtualCamera() { g_virtualCameraActive.store(false); }
bool WriteVirtualCameraFrame(const uint8_t*, uint32_t, uint32_t, uint64_t) { return false; }
bool WriteVirtualCameraFrameNV12(const uint8_t*, uint32_t, uint32_t, uint64_t) { return false; }
bool WriteVirtualCameraFrameNV12Planes(const uint8_t*, const uint8_t*, uint32_t, uint32_t, uint64_t) { return false; }
bool AcquireVirtualCameraGpuFrame(uint64_t, VirtualCameraGpuFrame&) { return false; }
bool PublishVirtualCameraGpuFrame(const VirtualCameraGpuFrame&) { return false; }
void AbandonVirtualCameraGpuFrame(const VirtualCameraGpuFrame&) {}
bool IsVirtualCameraActive() { return false; }
bool IsVirtualCameraDriverInstalled() { return false; }
bool IsVirtualCameraInUseByOBS() { return false; }
bool ShouldCaptureVirtualCameraFrame() { return false; }
bool EnsureVirtualCameraSize(uint32_t, uint32_t) { return false; }

bool GetVirtualCameraResolution(uint32_t& outWidth, uint32_t& outHeight) {
    outWidth = 0;
    outHeight = 0;
    return false;
}

void GetVirtualCameraMonitorSize(uint32_t& outWidth, uint32_t& outHeight) {
    outWidth = 0;
    outHeight = 0;
}

bool GetPreferredVirtualCameraResolution(uint32_t& outWidth, uint32_t& outHeight) {
    outWidth = 0;
    outHeight = 0;
    return false;
}

void OnGameWindowResized(uint32_t, uint32_t) {}
bool FlushPendingVirtualCameraResize() { return false; }
void RequestVirtualCameraRecoveryFrames() {}
std::string GetVirtualCameraError() { return "The OBS Virtual Camera driver is not available on macOS."; }
