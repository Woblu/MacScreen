#pragma once

// On Windows, Toolscreen draws into the game's own context and its default
// framebuffer. On macOS it draws into a separate core-profile context whose
// "screen" is an IOSurface-backed FBO (see gl_bridge.mm). These wrappers make
// that FBO behave like framebuffer 0 for the shared rendering code:
//   - binding framebuffer 0 binds the screen FBO
//   - framebuffer binding queries report 0 for the screen FBO
//   - GL_BACK/GL_FRONT draw/read buffers map to its color attachment
// Outside Toolscreen's context the calls pass straight through.

#include <GL/glew.h>

extern "C" {
void TsMacGlBindFramebuffer(GLenum target, GLuint framebuffer);
void TsMacGlGetIntegerv(GLenum pname, GLint* data);
void TsMacGlDrawBuffer(GLenum buffer);
void TsMacGlReadBuffer(GLenum buffer);
void TsMacGlDrawBuffers(GLsizei count, const GLenum* buffers);
}

#undef glBindFramebuffer
#define glBindFramebuffer TsMacGlBindFramebuffer
#define glGetIntegerv TsMacGlGetIntegerv
#define glDrawBuffer TsMacGlDrawBuffer
#define glReadBuffer TsMacGlReadBuffer
#undef glDrawBuffers
#define glDrawBuffers TsMacGlDrawBuffers
