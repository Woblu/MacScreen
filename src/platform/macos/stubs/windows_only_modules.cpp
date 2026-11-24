// macOS definitions for Windows-only modules:
//   - Vulkan renderer / layer: Minecraft 26.2+ Vulkan path (Windows-only).
//   - HookChain: chaining behind other in-process wglSwapBuffers detours
//     (OBS game capture, Discord); nothing injects into the game on macOS.
//   - GameTest: the in-game test harness driven by the Windows injector.

#include "hooks/hook_chain.h"
#include "render/vulkan/vulkan_hooks.h"
#include "render/vulkan/vulkan_renderer.h"
#include "testing/game_test_mode.h"

#include <dlfcn.h>

#include <sstream>

namespace VulkanHooks {

bool InstallIfAvailable() { return false; }
bool EnableProcessLocalObsRedirectLayer(HMODULE) { return false; }
void NotifyModuleLoaded(HMODULE) {}
FARPROC InterceptLoaderGetProcAddress(HMODULE, LPCSTR, FARPROC realFunction) { return realFunction; }
void Shutdown() {}

} // namespace VulkanHooks

namespace VulkanRenderer {

bool IsReady() { return false; }
bool GetColorPickerFrame(uintptr_t&, int&, int&) { return false; }
bool TryGetColorPickerSample(int, int, std::array<float, 4>&) { return false; }
bool GetBundledGuiTexture(int, uintptr_t&) { return false; }
bool GetGuiRgbaTexture(const std::string&, const unsigned char*, int, int, uint64_t, uintptr_t&) { return false; }

} // namespace VulkanRenderer

namespace HookChain {

bool TryCreateAndEnableHook(void*, void*, void**, const char*) { return false; }
bool IsAllowedSwapBuffersThirdPartyHookAddress(const void*) { return false; }
void RefreshAllThirdPartyHookChains() {}

std::string DescribeAddressWithOwner(const void* addr) {
    std::ostringstream ss;
    ss << addr;
    Dl_info info{};
    if (addr && dladdr(addr, &info) != 0 && info.dli_fname) ss << " (" << info.dli_fname << ")";
    return ss.str();
}

} // namespace HookChain

namespace GameTest {

void InitializeFromEnvironment() {}
void StartRunner(HWND) {}
void OnRenderThreadFrame() {}
void Shutdown() {}
void RecordDetachComplete() {}
bool ShouldForceSharedObsFrame() { return false; }

} // namespace GameTest
