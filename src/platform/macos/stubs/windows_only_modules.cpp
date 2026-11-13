// macOS definitions for Windows-only modules:
//   - Vulkan renderer / layer: Minecraft 26.2+ Vulkan path (Windows-only).
//   - HookChain: chaining behind other in-process wglSwapBuffers detours
//     (OBS game capture, Discord); nothing injects into the game on macOS.
//   - GameTest: the in-game test harness driven by the Windows injector.

#include "hooks/hook_chain.h"
#include "render/vulkan/vulkan_hooks.h"
#include "render/vulkan/vulkan_renderer.h"
#include "testing/game_test_mode.h"
#include "testing/thread_stack_dump.h"

#include <dlfcn.h>

#include <sstream>

namespace VulkanHooks {

std::shared_ptr<const TrackingSnapshot> GetSnapshot() { return std::make_shared<TrackingSnapshot>(); }
bool InstallIfAvailable() { return false; }
bool EnableProcessLocalObsRedirectLayer(HMODULE) { return false; }
void NotifyModuleLoaded(HMODULE) {}
FARPROC InterceptLoaderGetProcAddress(HMODULE, LPCSTR, FARPROC realFunction) { return realFunction; }
void Shutdown() {}
PFN_vkVoidFunction LoadRealFunction(const char*, void*) { return nullptr; }

} // namespace VulkanHooks

namespace VulkanRenderer {

bool RecordAfterFinalBlit(const FinalBlitContext&, PFN_vkCmdBlitImage) { return false; }
bool IsReady() { return false; }
FrameTrackingStats GetFrameTrackingStats() { return {}; }
void SetFrameTrackingProbeEnabled(bool) {}
StreamingTextureStats GetStreamingTextureStats(const std::string&) { return {}; }
bool GetColorPickerFrame(uintptr_t&, int&, int&) { return false; }
void RequestColorPickerSample(int, int) {}
bool TryGetColorPickerSample(int, int, std::array<float, 4>&) { return false; }
bool GetBundledGuiTexture(int, uintptr_t&) { return false; }
bool GetGuiRgbaTexture(const std::string&, const unsigned char*, int, int, uint64_t, uintptr_t&) { return false; }
void OnQueueSubmit(VkDevice, VkQueue, uint32_t, const VkCommandBuffer*, VkFence) {}
void OnQueuePresent(VkDevice, VkQueue, const VkPresentInfoKHR*, const std::vector<VkImage>&) {}
void OnImageDestroyed(VkDevice, VkImage) {}
void OnSwapchainDestroyed(VkDevice, VkSwapchainKHR, const std::vector<VkImage>&) {}
void OnDeviceDestroyed(VkDevice) {}
void Shutdown() {}
bool GetMirrorHasContentForTests(const std::string&, bool&) { return false; }
EyeZoomSnapshotStats GetEyeZoomSnapshotStatsForTests() { return {}; }
bool TryGetPresentedPixelForTests(int, int, std::array<float, 4>&) { return false; }
void RequestFrameCaptureForTests() {}
bool TryGetFrameCaptureForTests(std::vector<uint8_t>&, int&, int&) { return false; }

} // namespace VulkanRenderer

namespace HookChain {

bool TryCreateAndEnableHook(void*, void*, void**, const char*) { return false; }
bool IsAllowedSwapBuffersThirdPartyHookAddress(const void*) { return false; }
void RefreshAllThirdPartyHookChains() {}
void* GetThirdPartyWglSwapBuffersHookTarget() { return nullptr; }

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
bool IsEnabled() { return false; }
void StartRunner(HWND) {}
void OnRenderThreadFrame() {}
void Shutdown() {}
void RecordDetachComplete() {}
bool ShouldForceSharedObsFrame() { return false; }
std::string DumpInterestingThreadStacks() { return {}; }

} // namespace GameTest
