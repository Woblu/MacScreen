#pragma once

// Wraps the real GLEW header. Toolscreen renders in its own core-profile
// context on macOS, where an IOSurface-backed framebuffer stands in for the
// window's default framebuffer; see render/gl_redirect.h.
#include_next <GL/glew.h>

#if defined(__cplusplus) && !defined(TS_MAC_GL_REDIRECT_IMPL)
#include "platform/macos/render/gl_redirect.h"
#endif
