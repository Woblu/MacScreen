#define TS_MAC_GL_REDIRECT_IMPL
#include <GL/glew.h>

#include "platform/macos/render/gl_bridge.h"

namespace {

bool IsDefaultLikeBuffer(GLenum buffer) {
    return buffer == GL_BACK || buffer == GL_BACK_LEFT || buffer == GL_FRONT || buffer == GL_FRONT_LEFT ||
           buffer == GL_FRONT_AND_BACK || buffer == GL_LEFT;
}

bool ScreenBoundAs(GLenum bindingQuery, GLuint screen) {
    GLint bound = 0;
    glGetIntegerv(bindingQuery, &bound);
    return screen != 0 && static_cast<GLuint>(bound) == screen;
}

} // namespace

extern "C" void TsMacGlBindFramebuffer(GLenum target, GLuint framebuffer) {
    if (framebuffer == 0 && MacGlBridge::IsToolscreenContextCurrent()) {
        framebuffer = MacGlBridge::ScreenFramebuffer();
    }
    glBindFramebuffer(target, framebuffer);
}

extern "C" void TsMacGlGetIntegerv(GLenum pname, GLint* data) {
    glGetIntegerv(pname, data);
    if (!data || !MacGlBridge::IsToolscreenContextCurrent()) return;

    const GLuint screen = MacGlBridge::ScreenFramebuffer();
    if (screen == 0) return;

    switch (pname) {
    case GL_DRAW_FRAMEBUFFER_BINDING:
    case GL_READ_FRAMEBUFFER_BINDING:
        if (static_cast<GLuint>(data[0]) == screen) data[0] = 0;
        break;
    case GL_DRAW_BUFFER:
    case GL_DRAW_BUFFER0:
        if (data[0] == GL_COLOR_ATTACHMENT0 && ScreenBoundAs(GL_DRAW_FRAMEBUFFER_BINDING, screen)) data[0] = GL_BACK;
        break;
    case GL_READ_BUFFER:
        if (data[0] == GL_COLOR_ATTACHMENT0 && ScreenBoundAs(GL_READ_FRAMEBUFFER_BINDING, screen)) data[0] = GL_BACK;
        break;
    default:
        break;
    }
}

extern "C" void TsMacGlDrawBuffer(GLenum buffer) {
    if (IsDefaultLikeBuffer(buffer) && MacGlBridge::IsToolscreenContextCurrent() &&
        ScreenBoundAs(GL_DRAW_FRAMEBUFFER_BINDING, MacGlBridge::ScreenFramebuffer())) {
        buffer = GL_COLOR_ATTACHMENT0;
    }
    glDrawBuffer(buffer);
}

extern "C" void TsMacGlReadBuffer(GLenum buffer) {
    if (IsDefaultLikeBuffer(buffer) && MacGlBridge::IsToolscreenContextCurrent() &&
        ScreenBoundAs(GL_READ_FRAMEBUFFER_BINDING, MacGlBridge::ScreenFramebuffer())) {
        buffer = GL_COLOR_ATTACHMENT0;
    }
    glReadBuffer(buffer);
}

extern "C" void TsMacGlDrawBuffers(GLsizei count, const GLenum* buffers) {
    if (count == 1 && buffers && IsDefaultLikeBuffer(buffers[0]) && MacGlBridge::IsToolscreenContextCurrent() &&
        ScreenBoundAs(GL_DRAW_FRAMEBUFFER_BINDING, MacGlBridge::ScreenFramebuffer())) {
        const GLenum attachment = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &attachment);
        return;
    }
    glDrawBuffers(count, buffers);
}
