#pragma once

#include <cstdint>

// Bridges Minecraft's OpenGL context and Toolscreen's own context.
//
// Minecraft 1.16 runs in a legacy (2.1) context on macOS, while Toolscreen's
// renderer needs GLSL 330 / GL 3.3. Each frame, inside the game's swap:
//   1. game context: the default framebuffer and the game's main render target
//      texture are copied into IOSurfaces
//   2. Toolscreen context (4.1 core): the screen IOSurface is bound as the
//      default framebuffer (see gl_redirect.h) and the render target copy is
//      exposed as an ordinary GL_TEXTURE_2D; the shared renderer runs unchanged
//   3. game context: the composed screen is copied back before the real swap
namespace MacGlBridge {

constexpr unsigned kNoTexture = 0xFFFFFFFFu;

// True on the current thread while Toolscreen's context is current.
bool IsToolscreenContextCurrent();

// FBO standing in for framebuffer 0 in Toolscreen's context (0 if none).
unsigned ScreenFramebuffer();

// Game context must be current. Captures the frame, makes Toolscreen's context
// current and returns the render target copy (or kNoTexture) through
// outGameTextureCopy. Returns false (and leaves the game context current) if
// the bridge is unavailable.
bool BeginToolscreenFrame(int framebufferWidth, int framebufferHeight, unsigned gameTexture, unsigned& outGameTextureCopy);

// Toolscreen context must be current. Copies the composed screen into the
// game's default framebuffer and makes the game context current again.
void EndToolscreenFrame();

// Game context current. Redraws the visible part of the game's frame, which
// the game drew into a viewport extending past its default framebuffer (e.g.
// EyeZoom's tall mode). Windows drivers clip such a viewport; on macOS the
// game's own draw showed the wrong part of the frame.
void RedrawClippedGameViewport(unsigned gameTexture, const int viewport[4], int framebufferWidth, int framebufferHeight);

// Creates Toolscreen's context next to the given game context and initializes
// GLEW in it. Safe to call repeatedly. Restores the previously current context.
bool EnsureToolscreenContext(void* gameCglContext);

// Toolscreen's CGL context, or nullptr before EnsureToolscreenContext succeeded.
void* ToolscreenContext();

// Makes the game's context current for its lifetime if Toolscreen's context is
// current on this thread. The game's callbacks can run synchronously from
// Toolscreen's frame (e.g. the settings GUI resizing the window delivers the
// game's framebuffer-size callback); on Windows there is only one context.
class ScopedGameContext {
public:
    ScopedGameContext();
    ~ScopedGameContext();
    ScopedGameContext(const ScopedGameContext&) = delete;
    ScopedGameContext& operator=(const ScopedGameContext&) = delete;

private:
    bool switched_ = false;
};

} // namespace MacGlBridge
