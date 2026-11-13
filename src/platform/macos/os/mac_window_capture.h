#pragma once

#include <cstddef>
#include <cstdint>

// Captures other applications' windows with ScreenCaptureKit. Used by the
// Win32 PrintWindow/BitBlt implementations that window overlays rely on.
namespace MacWindowCapture {

// Copies the newest frame of `windowId` (a CGWindowID), scaled to
// width x height, into `bgra` (top-down rows of `stride` bytes). Capture
// starts on the first request and stops when the window is no longer
// requested. Returns false until a frame is available.
bool CopyFrame(uint32_t windowId, int width, int height, uint8_t* bgra, size_t stride);

} // namespace MacWindowCapture
