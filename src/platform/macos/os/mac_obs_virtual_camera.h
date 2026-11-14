#pragma once

// Feeds OBS Studio's macOS virtual camera (its CoreMediaIO camera extension)
// from the "OBSVirtualCamVideo" shared-memory queue that
// src/features/virtual_camera.cpp writes. On Windows, OBS's camera driver reads
// that queue; here this bridge does, and hands each frame to the extension's
// sink stream the way OBS's own macOS plugin does.

#include <cstddef>

namespace MacObsVirtualCamera {

// True when OBS's camera extension device is present (OBS installs it the
// first time its virtual camera is started). Checked in the background; the
// result can lag by up to two seconds.
bool IsAvailable();

// Called when the queue's section is created and right before it is unmapped.
void Attach(void* base, size_t size);
void Detach(void* base);

} // namespace MacObsVirtualCamera
