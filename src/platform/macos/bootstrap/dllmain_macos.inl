// macOS glue for bootstrap/dllmain.cpp. Included at the end of that file so it
// can reach the hook functions and state defined there.
//
// On Windows, DllMain installs MinHook detours once the game window exists.
// On macOS every hook is registered by name before LWJGL resolves its
// functions (see hooks/symbol_hooks.h); the detours are the same functions.

#include "platform/macos/hooks/glfw_bridge.h"
#include "platform/macos/hooks/symbol_hooks.h"
#include "platform/macos/render/gl_bridge.h"
#include "platform/macos/win32/user32_internal.h"
#include "platform/macos/win32/win32_internal.h"

#include <dlfcn.h>
#include <unistd.h>

namespace {

void* g_macSwapWindow = nullptr;
GLuint g_macGameTexture = UINT_MAX;
GLBINDFRAMEBUFFERPROC g_macOglBindFramebufferExt = nullptr;

void APIENTRY MacHkBindFramebufferExt(GLenum target, GLuint framebuffer) {
    if (!g_macOglBindFramebufferExt) return;
    BindFramebufferHook_Impl(g_macOglBindFramebufferExt, target, framebuffer);
}

HWND MacGetWindowHandle(void* window) { return static_cast<HWND>(window); }

// Upstream's "real SwapBuffers": present Toolscreen's frame, then swap.
BOOL WINAPI MacSwapTail(HDC) {
    MacGlBridge::EndToolscreenFrame();
    MacGlfwBridge::RealSwapBuffers(g_macSwapWindow);
    return TRUE;
}

void MacSwapBuffersDetour(void* window) {
    if (!window || window != MacWin32::GameWindow()) {
        MacGlfwBridge::RealSwapBuffers(window);
        return;
    }
    if (!glfwGetCurrentContextProc.load(std::memory_order_acquire)) {
        glfwGetCurrentContextProc.store(reinterpret_cast<GLFWGETCURRENTCONTEXTPROC>(MacSymbolHooks::ResolveGlfw("glfwGetCurrentContext")),
                                        std::memory_order_release);
    }

    MacGlfwBridge::BeforeSwap(window);

    int width = 0;
    int height = 0;
    if (!MacGlfwBridge::RealFramebufferSize(window, width, height)) {
        MacGlfwBridge::RealSwapBuffers(window);
        return;
    }

    // Same cached-texture rule as SwapBuffersHook_Impl, kept in the game
    // context's texture namespace.
    const GLuint trackedFramebuffer = g_lastTrackedGameFramebufferTextureId.load(std::memory_order_acquire);
    const GLuint trackedBind = g_lastTrackedGameTextureBindId.load(std::memory_order_acquire);
    const GLuint tracked = trackedFramebuffer != UINT_MAX ? trackedFramebuffer : trackedBind;
    if (g_cachedGameTextureId.load(std::memory_order_acquire) == UINT_MAX) g_macGameTexture = UINT_MAX;
    if (tracked != UINT_MAX) g_macGameTexture = tracked;

    unsigned copy = MacGlBridge::kNoTexture;
    if (!MacGlBridge::BeginToolscreenFrame(width, height, g_macGameTexture, copy)) {
        MacGlfwBridge::RealSwapBuffers(window);
        return;
    }

    // Toolscreen's renderer samples the copy for this frame.
    const GLuint substitute = (copy != MacGlBridge::kNoTexture) ? static_cast<GLuint>(copy) : UINT_MAX;
    g_lastTrackedGameFramebufferTextureId.store(UINT_MAX, std::memory_order_release);
    g_lastTrackedGameTextureBindId.store(substitute, std::memory_order_release);

    g_macSwapWindow = window;
    hkwglSwapBuffers(GetDC(static_cast<HWND>(window)));

    // Put the game-context ids back unless the logic thread invalidated them.
    GLuint expected = substitute;
    if (g_lastTrackedGameTextureBindId.compare_exchange_strong(expected, trackedBind, std::memory_order_acq_rel)) {
        g_lastTrackedGameFramebufferTextureId.store(trackedFramebuffer, std::memory_order_release);
    }
}

} // namespace

void MacInstallHooks() {
    // OpenGL entry points (resolved by LWJGL through the OpenGL bundle).
    MacSymbolHooks::RegisterGlHook("glViewport", reinterpret_cast<void*>(&hkglViewport), reinterpret_cast<void**>(&oglViewport));
    g_glViewportHookCount.fetch_add(1);
    MacSymbolHooks::RegisterGlHook("glBindTexture", reinterpret_cast<void*>(&hkglBindTexture), reinterpret_cast<void**>(&oglBindTexture));
    MacSymbolHooks::RegisterGlHook("glBindFramebuffer", reinterpret_cast<void*>(&hkglBindFramebuffer),
                                   reinterpret_cast<void**>(&oglBindFramebuffer));
    MacSymbolHooks::RegisterGlHook("glBindFramebufferEXT", reinterpret_cast<void*>(&MacHkBindFramebufferExt),
                                   reinterpret_cast<void**>(&g_macOglBindFramebufferExt));
    if (MacSymbolHooks::RegisterGlHook("glBlitFramebuffer", reinterpret_cast<void*>(&hkglBlitFramebuffer),
                                       reinterpret_cast<void**>(&oglBlitFramebuffer))) {
        g_glBlitFramebufferHooked.store(true, std::memory_order_release);
    }

    // GLFW. Upstream's detours stay in front; their "original" pointers go
    // through the bridge, which talks to the real GLFW.
    glfwGetWin32WindowProc = &MacGetWindowHandle;
    oglfwCreateWindow = &MacGlfwBridge::CreateWindowThroughBridge;
    oglfwDestroyWindow = &MacGlfwBridge::DestroyWindowThroughBridge;
    oglfwSetInputMode = &MacGlfwBridge::SetInputModeThroughBridge;
    oglfwSetCursor = reinterpret_cast<GLFWSETCURSOR>(&MacGlfwBridge::SetCursorThroughBridge);
    oglfwSetWindowSizeCallback = &MacGlfwBridge::SetWindowSizeCallbackThroughBridge;
    oglfwSetFramebufferSizeCallback = &MacGlfwBridge::SetFramebufferSizeCallbackThroughBridge;
    MacGlfwBridge::SetUpstreamSizeCallbackSetters(&hkglfwSetWindowSizeCallback, &hkglfwSetFramebufferSizeCallback);

    MacSymbolHooks::RegisterDlsymHook("glfwCreateWindow", reinterpret_cast<void*>(&hkglfwCreateWindow), MacGlfwBridge::RealCreateWindowSlot());
    MacSymbolHooks::RegisterDlsymHook("glfwWindowHint", reinterpret_cast<void*>(&hkglfwWindowHint), reinterpret_cast<void**>(&oglfwWindowHint));
    MacSymbolHooks::RegisterDlsymHook("glfwDestroyWindow", reinterpret_cast<void*>(&hkglfwDestroyWindow),
                                      MacGlfwBridge::RealDestroyWindowSlot());
    MacSymbolHooks::RegisterDlsymHook("glfwSetInputMode", reinterpret_cast<void*>(&hkglfwSetInputMode), MacGlfwBridge::RealSetInputModeSlot());
    MacSymbolHooks::RegisterDlsymHook("glfwSetCursor", reinterpret_cast<void*>(&hkglfwSetCursor), MacGlfwBridge::RealSetCursorSlot());
    MacSymbolHooks::RegisterDlsymHook("glfwSwapBuffers", reinterpret_cast<void*>(&MacSwapBuffersDetour), MacGlfwBridge::RealSwapBuffersSlot());

    // Swap, cursor and sensitivity plumbing.
    owglSwapBuffers = &MacSwapTail;
    oSetCursorPos = &MacGlfwBridge::WarpCursorScreen;
    MacGlfwBridge::SetCursorPosHook(&hkSetCursorPos);
    MacGlfwBridge::SetSensitivityResolver(&ResolveMouseSensitivity);
    MacGlfwBridge::SetGuiOpenQuery([] { return g_showGui.load(std::memory_order_acquire); });
    MacGlfwBridge::RegisterHooks();

    MacSymbolHooks::Install();
    g_gameWindowHooksReady.store(true, std::memory_order_release);
    LogCategory("init", "macOS hooks registered; they attach when LWJGL loads.");
}

// ---------------------------------------------------------------------------------------------
// Library entry points
// ---------------------------------------------------------------------------------------------

namespace MacWin32 {
HMODULE ToolscreenModule() {
    static HMODULE module = [] {
        Dl_info info{};
        dladdr(reinterpret_cast<const void*>(&MacInstallHooks), &info);
        return static_cast<HMODULE>(info.dli_fbase);
    }();
    return module;
}
} // namespace MacWin32

namespace {

// liblogger records every native library loaded into the game for run
// verification. It is shipped next to Toolscreen and loaded before anything
// else, as the Windows injector does.
void LoadLibLogger() {
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&MacInstallHooks), &info) == 0 || !info.dli_fname) return;
    std::string path = info.dli_fname;
    const size_t slash = path.find_last_of('/');
    path = (slash == std::string::npos ? std::string() : path.substr(0, slash + 1)) + "liblogger.dylib";
    if (access(path.c_str(), R_OK) == 0) dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
}

} // namespace

// Toolscreen is loaded with the JVM option -agentpath. The JVM calls these once,
// before the game starts and while it shuts down; they take the place of
// DllMain's process attach and process exit.
extern "C" __attribute__((visibility("default"))) int Agent_OnLoad(void*, char*, void*) {
    LoadLibLogger();
    DllMain(MacWin32::ToolscreenModule(), DLL_PROCESS_ATTACH, nullptr);
    return 0;
}

extern "C" __attribute__((visibility("default"))) void Agent_OnUnload(void*) {
    DllMain(MacWin32::ToolscreenModule(), DLL_PROCESS_DETACH, reinterpret_cast<LPVOID>(1));
}
