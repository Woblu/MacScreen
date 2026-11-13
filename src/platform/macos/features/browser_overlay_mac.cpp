// Browser overlays on macOS. Configuration handling matches the Windows
// implementation; page rendering is not available yet, so overlays show no
// frame.

#include "features/browser_overlay.h"

#include "common/utils.h"

void StartBrowserOverlayThread() {}
void StopBrowserOverlayThread() {}
void CleanupBrowserOverlayCache() {}
void RemoveBrowserOverlayFromCache(const std::string&) {}
void RequestBrowserOverlayRefresh(const std::string&) {}

const BrowserOverlayConfig* FindBrowserOverlayConfig(const std::string& overlayId) {
    for (const auto& overlay : g_config.browserOverlays) {
        if (overlay.name == overlayId) return &overlay;
    }
    return nullptr;
}

const BrowserOverlayConfig* FindBrowserOverlayConfigIn(const std::string& overlayId, const Config& config) {
    for (const auto& overlay : config.browserOverlays) {
        if (overlay.name == overlayId) return &overlay;
    }
    return nullptr;
}

bool StageBrowserOverlayTestFrame(const BrowserOverlayConfig&, const std::vector<unsigned char>&, int, int) { return false; }
bool PrepareBrowserOverlayTexture(const BrowserOverlayConfig&, BrowserOverlayTextureFrame&) { return false; }
bool AcquireBrowserOverlayPixelFrame(const BrowserOverlayConfig&, BrowserOverlayPixelFrame&) { return false; }
