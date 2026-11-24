// Game-context half of the bridge. Uses the system OpenGL headers and the EXT
// framebuffer entry points, which are valid in Minecraft's legacy context.

#import <Foundation/Foundation.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/CGLIOSurface.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <IOSurface/IOSurface.h>

#include "platform/macos/render/gl_bridge.h"
#include "platform/macos/render/gl_bridge_internal.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

namespace {

struct SharedSurface {
    IOSurfaceRef surface = nullptr;
    int width = 0;
    int height = 0;
    // Objects in the game context.
    GLuint gameTexture = 0;
    GLuint gameFramebuffer = 0;
    // Objects in Toolscreen's context.
    MacGlBridgeCore::SurfaceTarget core;
    bool coreBound = false;
};

CGLContextObj g_toolscreenContext = nullptr;
CGLContextObj g_gameContext = nullptr;
std::mutex g_contextMutex;
std::atomic<unsigned> g_screenFramebuffer{ 0 };
thread_local bool t_toolscreenCurrent = false;

SharedSurface g_screen;
SharedSurface g_gameCopy;
MacGlBridgeCore::Texture2DTarget g_gameTexture2D;
GLuint g_gameReadFramebuffer = 0;
bool g_gameCopyValid = false;

IOSurfaceRef CreateSurface(int width, int height) {
    @autoreleasepool {
    const int bytesPerElement = 4;
    NSDictionary* properties = @{
        (id)kIOSurfaceWidth : @(width),
        (id)kIOSurfaceHeight : @(height),
        (id)kIOSurfaceBytesPerElement : @(bytesPerElement),
        (id)kIOSurfaceBytesPerRow : @(IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, static_cast<size_t>(width) * bytesPerElement)),
        (id)kIOSurfacePixelFormat : @((uint32_t)'BGRA'),
    };
    return IOSurfaceCreate((__bridge CFDictionaryRef)properties);
    }
}

struct GameStateGuard {
    GLint readFramebuffer = 0;
    GLint drawFramebuffer = 0;
    GLint rectangleTexture = 0;
    GLboolean scissor = GL_FALSE;

    GameStateGuard() {
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING_EXT, &readFramebuffer);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING_EXT, &drawFramebuffer);
        glGetIntegerv(GL_TEXTURE_BINDING_RECTANGLE_ARB, &rectangleTexture);
        scissor = glIsEnabled(GL_SCISSOR_TEST);
        if (scissor) glDisable(GL_SCISSOR_TEST);
    }

    ~GameStateGuard() {
        glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, static_cast<GLuint>(readFramebuffer));
        glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, static_cast<GLuint>(drawFramebuffer));
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, static_cast<GLuint>(rectangleTexture));
        if (scissor) glEnable(GL_SCISSOR_TEST);
    }
};

// Game context current. (Re)creates the surface and its game-side objects.
bool EnsureGameSide(SharedSurface& s, int width, int height) {
    if (width <= 0 || height <= 0) return false;
    if (s.surface && s.width == width && s.height == height && s.gameFramebuffer != 0) return true;

    if (s.surface) {
        CFRelease(s.surface);
        s.surface = nullptr;
    }
    s.surface = CreateSurface(width, height);
    if (!s.surface) return false;
    s.width = width;
    s.height = height;
    s.coreBound = false;

    if (s.gameTexture == 0) glGenTextures(1, &s.gameTexture);
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, s.gameTexture);
    const CGLError err = CGLTexImageIOSurface2D(g_gameContext, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA8, width, height, GL_BGRA,
                                                GL_UNSIGNED_INT_8_8_8_8_REV, s.surface, 0);
    if (err != kCGLNoError) return false;

    if (s.gameFramebuffer == 0) glGenFramebuffersEXT(1, &s.gameFramebuffer);
    glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, s.gameFramebuffer);
    glFramebufferTexture2DEXT(GL_DRAW_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_RECTANGLE_ARB, s.gameTexture, 0);
    return glCheckFramebufferStatusEXT(GL_DRAW_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT;
}

bool TextureSize(GLuint texture, int& width, int& height) {
    width = 0;
    height = 0;
    if (texture == 0 || texture == MacGlBridge::kNoTexture || !glIsTexture(texture)) return false;
    GLint previous = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
    glBindTexture(GL_TEXTURE_2D, texture);
    GLint w = 0;
    GLint h = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous));
    width = w;
    height = h;
    return w > 0 && h > 0;
}

CGLContextObj CreateToolscreenContext(CGLContextObj game) {
    GLint rendererId = 0;
    const bool haveRenderer = CGLGetParameter(game, kCGLCPCurrentRendererID, &rendererId) == kCGLNoError && rendererId != 0;

    CGLPixelFormatAttribute attributes[16];
    int i = 0;
    attributes[i++] = kCGLPFAOpenGLProfile;
    attributes[i++] = static_cast<CGLPixelFormatAttribute>(kCGLOGLPVersion_GL4_Core);
    attributes[i++] = kCGLPFAColorSize;
    attributes[i++] = static_cast<CGLPixelFormatAttribute>(24);
    attributes[i++] = kCGLPFAAlphaSize;
    attributes[i++] = static_cast<CGLPixelFormatAttribute>(8);
    attributes[i++] = kCGLPFAAccelerated;
    attributes[i++] = kCGLPFAAllowOfflineRenderers;
    if (haveRenderer) {
        attributes[i++] = kCGLPFARendererID;
        attributes[i++] = static_cast<CGLPixelFormatAttribute>(rendererId);
    }
    attributes[i] = static_cast<CGLPixelFormatAttribute>(0);

    CGLPixelFormatObj format = nullptr;
    GLint formatCount = 0;
    if (CGLChoosePixelFormat(attributes, &format, &formatCount) != kCGLNoError || !format) {
        if (!haveRenderer) return nullptr;
        attributes[i - 2] = static_cast<CGLPixelFormatAttribute>(0);
        if (CGLChoosePixelFormat(attributes, &format, &formatCount) != kCGLNoError || !format) return nullptr;
    }

    CGLContextObj context = nullptr;
    const CGLError err = CGLCreateContext(format, nullptr, &context);
    CGLReleasePixelFormat(format);
    return err == kCGLNoError ? context : nullptr;
}

bool BindCoreSurface(SharedSurface& s) {
    if (s.coreBound) return true;
    s.coreBound = MacGlBridgeCore::BindIOSurface(s.core, g_toolscreenContext, s.surface, s.width, s.height);
    return s.coreBound;
}

} // namespace

namespace MacGlBridge {

bool IsToolscreenContextCurrent() { return t_toolscreenCurrent; }

unsigned ScreenFramebuffer() { return g_screenFramebuffer.load(std::memory_order_acquire); }

void* ToolscreenContext() { return g_toolscreenContext; }

ScopedGameContext::ScopedGameContext() {
    if (!t_toolscreenCurrent || !g_gameContext) return;
    glFlush();
    CGLSetCurrentContext(g_gameContext);
    t_toolscreenCurrent = false;
    switched_ = true;
}

ScopedGameContext::~ScopedGameContext() {
    if (!switched_) return;
    glFlush();
    CGLSetCurrentContext(g_toolscreenContext);
    t_toolscreenCurrent = true;
}

bool EnsureToolscreenContext(void* gameCglContext) {
    std::lock_guard<std::mutex> lock(g_contextMutex);
    if (g_toolscreenContext) return true;
    CGLContextObj game = static_cast<CGLContextObj>(gameCglContext);
    if (!game) return false;

    CGLContextObj context = CreateToolscreenContext(game);
    if (!context) return false;

    CGLContextObj previous = CGLGetCurrentContext();
    CGLSetCurrentContext(context);
    const bool glewReady = MacGlBridgeCore::InitializeGlew();
    CGLSetCurrentContext(previous);
    if (!glewReady) {
        CGLDestroyContext(context);
        return false;
    }
    g_toolscreenContext = context;
    return true;
}

bool BeginToolscreenFrame(int framebufferWidth, int framebufferHeight, unsigned gameTexture, unsigned& outGameTextureCopy) {
    outGameTextureCopy = kNoTexture;
    CGLContextObj game = CGLGetCurrentContext();
    if (!game || game == g_toolscreenContext || framebufferWidth <= 0 || framebufferHeight <= 0) return false;
    if (!EnsureToolscreenContext(game)) return false;

    if (g_gameContext != game) {
        // A new game context (e.g. after a renderer change) owns none of our objects.
        g_gameContext = game;
        g_screen = SharedSurface{};
        g_gameCopy = SharedSurface{};
        g_gameReadFramebuffer = 0;
    }

    // On Windows, Toolscreen reads the game's last viewport (e.g. for overlay
    // dragging); Toolscreen's own context starts with a 0x0 one.
    GLint gameViewport[4] = { 0, 0, framebufferWidth, framebufferHeight };
    glGetIntegerv(GL_VIEWPORT, gameViewport);

    {
        GameStateGuard guard;
        if (!EnsureGameSide(g_screen, framebufferWidth, framebufferHeight)) return false;

        glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, 0);
        glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, g_screen.gameFramebuffer);
        glBlitFramebufferEXT(0, 0, framebufferWidth, framebufferHeight, 0, 0, framebufferWidth, framebufferHeight,
                             GL_COLOR_BUFFER_BIT, GL_NEAREST);

        g_gameCopyValid = false;
        int textureWidth = 0;
        int textureHeight = 0;
        if (TextureSize(gameTexture, textureWidth, textureHeight) && EnsureGameSide(g_gameCopy, textureWidth, textureHeight)) {
            if (g_gameReadFramebuffer == 0) glGenFramebuffersEXT(1, &g_gameReadFramebuffer);
            glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, g_gameReadFramebuffer);
            glFramebufferTexture2DEXT(GL_READ_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, gameTexture, 0);
            if (glCheckFramebufferStatusEXT(GL_READ_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT) {
                glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, g_gameCopy.gameFramebuffer);
                glBlitFramebufferEXT(0, 0, textureWidth, textureHeight, 0, 0, textureWidth, textureHeight, GL_COLOR_BUFFER_BIT,
                                     GL_NEAREST);
                g_gameCopyValid = true;
            }
            glFramebufferTexture2DEXT(GL_READ_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, 0, 0);
        }
    }
    glFlush();

    CGLSetCurrentContext(g_toolscreenContext);
    t_toolscreenCurrent = true;

    if (!BindCoreSurface(g_screen)) {
        t_toolscreenCurrent = false;
        CGLSetCurrentContext(game);
        return false;
    }
    g_screenFramebuffer.store(g_screen.core.framebuffer, std::memory_order_release);

    if (g_gameCopyValid && BindCoreSurface(g_gameCopy) &&
        MacGlBridgeCore::EnsureTexture2D(g_gameTexture2D, g_gameCopy.width, g_gameCopy.height)) {
        MacGlBridgeCore::CopySurfaceToTexture(g_gameCopy.core, g_gameTexture2D);
        outGameTextureCopy = g_gameTexture2D.texture;
    }

    MacGlBridgeCore::BindScreen(g_screen.core);
    glViewport(gameViewport[0], gameViewport[1], gameViewport[2], gameViewport[3]);
    return true;
}

void EndToolscreenFrame() {
    if (!t_toolscreenCurrent) return;
    MacGlBridgeCore::Flush();
    t_toolscreenCurrent = false;
    CGLSetCurrentContext(g_gameContext);

    GameStateGuard guard;
    glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, g_screen.gameFramebuffer);
    glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, 0);
    glBlitFramebufferEXT(0, 0, g_screen.width, g_screen.height, 0, 0, g_screen.width, g_screen.height, GL_COLOR_BUFFER_BIT,
                         GL_NEAREST);
}

void RedrawClippedGameViewport(unsigned gameTexture, const int viewport[4], int framebufferWidth, int framebufferHeight) {
    if (CGLGetCurrentContext() != g_gameContext || viewport[2] <= 0 || viewport[3] <= 0) return;
    const int left = std::max(viewport[0], 0);
    const int bottom = std::max(viewport[1], 0);
    const int right = std::min(viewport[0] + viewport[2], framebufferWidth);
    const int top = std::min(viewport[1] + viewport[3], framebufferHeight);
    int textureWidth = 0;
    int textureHeight = 0;
    if (right <= left || top <= bottom || !TextureSize(gameTexture, textureWidth, textureHeight)) return;

    // Texture coordinate of a framebuffer position inside the viewport.
    const auto source = [](int offset, int viewportSize, int textureSize) {
        return static_cast<GLint>(std::lround(static_cast<double>(offset) * textureSize / viewportSize));
    };
    GameStateGuard guard;
    if (g_gameReadFramebuffer == 0) glGenFramebuffersEXT(1, &g_gameReadFramebuffer);
    glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, g_gameReadFramebuffer);
    glFramebufferTexture2DEXT(GL_READ_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, gameTexture, 0);
    if (glCheckFramebufferStatusEXT(GL_READ_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT) {
        glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, 0);
        glBlitFramebufferEXT(source(left - viewport[0], viewport[2], textureWidth), source(bottom - viewport[1], viewport[3], textureHeight),
                             source(right - viewport[0], viewport[2], textureWidth), source(top - viewport[1], viewport[3], textureHeight),
                             left, bottom, right, top, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    glFramebufferTexture2DEXT(GL_READ_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, 0, 0);
}

} // namespace MacGlBridge
