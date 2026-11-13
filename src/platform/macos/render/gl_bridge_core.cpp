#define TS_MAC_GL_REDIRECT_IMPL
#include <GL/glew.h>

#include <OpenGL/CGLIOSurface.h>
#include <OpenGL/OpenGL.h>

#include "platform/macos/render/gl_bridge_internal.h"

namespace MacGlBridgeCore {

bool InitializeGlew() {
    glewExperimental = GL_TRUE;
    const GLenum result = glewInit();
    // GLEW may leave GL_INVALID_ENUM from probing extensions on core profiles.
    while (glGetError() != GL_NO_ERROR) {
    }
    return result == GLEW_OK;
}

bool BindIOSurface(SurfaceTarget& target, void* cglContext, void* ioSurface, int width, int height) {
    if (!cglContext || !ioSurface || width <= 0 || height <= 0) return false;

    GLint previousTexture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_RECTANGLE, &previousTexture);

    if (target.texture == 0) glGenTextures(1, &target.texture);
    glBindTexture(GL_TEXTURE_RECTANGLE, target.texture);
    const CGLError err = CGLTexImageIOSurface2D(static_cast<CGLContextObj>(cglContext), GL_TEXTURE_RECTANGLE, GL_RGBA8, width,
                                                height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                                                static_cast<IOSurfaceRef>(ioSurface), 0);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_RECTANGLE, static_cast<GLuint>(previousTexture));
    if (err != kCGLNoError) return false;

    GLint previousDraw = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDraw);
    if (target.framebuffer == 0) glGenFramebuffers(1, &target.framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target.framebuffer);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, target.texture, 0);
    const GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDraw));

    target.width = width;
    target.height = height;
    return status == GL_FRAMEBUFFER_COMPLETE;
}

bool EnsureTexture2D(Texture2DTarget& target, int width, int height) {
    if (width <= 0 || height <= 0) return false;
    if (target.texture != 0 && target.width == width && target.height == height) return true;

    GLint previousTexture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    if (target.texture == 0) glGenTextures(1, &target.texture);
    glBindTexture(GL_TEXTURE_2D, target.texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));

    GLint previousDraw = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDraw);
    if (target.framebuffer == 0) glGenFramebuffers(1, &target.framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target.framebuffer);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture, 0);
    const GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDraw));

    target.width = width;
    target.height = height;
    return status == GL_FRAMEBUFFER_COMPLETE;
}

void CopySurfaceToTexture(const SurfaceTarget& source, const Texture2DTarget& dest) {
    if (source.framebuffer == 0 || dest.framebuffer == 0) return;

    GLint previousRead = 0;
    GLint previousDraw = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDraw);
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
    if (scissor) glDisable(GL_SCISSOR_TEST);
    if (srgb) glDisable(GL_FRAMEBUFFER_SRGB);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, source.framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dest.framebuffer);
    glBlitFramebuffer(0, 0, source.width, source.height, 0, 0, dest.width, dest.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousRead));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDraw));
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (srgb) glEnable(GL_FRAMEBUFFER_SRGB);
}

void BindScreen(const SurfaceTarget& screen) {
    glBindFramebuffer(GL_FRAMEBUFFER, screen.framebuffer);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
}

void Flush() { glFlush(); }

} // namespace MacGlBridgeCore
