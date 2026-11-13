#pragma once

// Core-context half of the bridge (gl_bridge_core.cpp, built against GLEW).
// Only called while Toolscreen's context is current.
namespace MacGlBridgeCore {

bool InitializeGlew();

// Rectangle texture backed by an IOSurface, plus an FBO with it attached.
struct SurfaceTarget {
    unsigned texture = 0;
    unsigned framebuffer = 0;
    int width = 0;
    int height = 0;
};

// Creates or resizes `target` so it is backed by `ioSurface`.
bool BindIOSurface(SurfaceTarget& target, void* cglContext, void* ioSurface, int width, int height);

// Ordinary GL_TEXTURE_2D (with FBO) that receives the render target copy.
struct Texture2DTarget {
    unsigned texture = 0;
    unsigned framebuffer = 0;
    int width = 0;
    int height = 0;
};

bool EnsureTexture2D(Texture2DTarget& target, int width, int height);

// Copies `source` into `dest` (same size) without disturbing other state.
void CopySurfaceToTexture(const SurfaceTarget& source, const Texture2DTarget& dest);

// Binds `screen` as both draw and read framebuffer.
void BindScreen(const SurfaceTarget& screen);

void Flush();

} // namespace MacGlBridgeCore
