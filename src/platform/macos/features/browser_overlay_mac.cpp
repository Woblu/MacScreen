// Browser overlays on macOS. This is src/features/browser_overlay.cpp with
// WebView2 replaced by MacBrowserView (WKWebView). WebKit reports events on the
// main thread; they are queued to the overlay thread, where WebView2 delivers
// them, so the cache entry state is handled exactly as on Windows.

#include "features/browser_overlay.h"

#include "common/utils.h"
#include "platform/macos/os/mac_browser_view.h"
#include "render/render.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

namespace {

constexpr size_t kBrowserOverlayMaxFrameBytes = 100ull * 1024ull * 1024ull;
constexpr size_t kBrowserOverlayUploadPboCount = 3;

struct BrowserOverlayRenderData {
    unsigned char* pixelData = nullptr;
    int width = 0;
    int height = 0;

    BrowserOverlayRenderData() = default;
    ~BrowserOverlayRenderData() {
        if (pixelData) {
            delete[] pixelData;
            pixelData = nullptr;
        }
    }

    BrowserOverlayRenderData(const BrowserOverlayRenderData&) = delete;
    BrowserOverlayRenderData& operator=(const BrowserOverlayRenderData&) = delete;
};

struct BrowserOverlayCacheEntry {
    std::string name;
    std::string url;
    std::string customCss;
    int browserWidth = 1280;
    int browserHeight = 720;
    int fps = 15;
    bool transparentBackground = false;
    bool muteAudio = true;
    bool hardwareAcceleration = true;
    bool allowSystemMediaKeys = true;
    bool reloadOnUpdate = false;
    int reloadInterval = 0;
    bool markedForRemoval = false;

    bool viewCreated = false;
    uint64_t viewGeneration = 0;
    bool hasSeenContentLoading = false;
    bool captureInFlight = false;
    bool controllerCreationPending = false;
    bool controllerCreationFailed = false;
    bool pendingReload = false;
    uint64_t latestQueuedDecodeSequence = 0;
    std::chrono::steady_clock::time_point lastCaptureTime{};
    std::chrono::steady_clock::time_point lastReloadTime{};

    std::unique_ptr<BrowserOverlayRenderData> writeBuffer = std::make_unique<BrowserOverlayRenderData>();
    std::unique_ptr<BrowserOverlayRenderData> readyBuffer = std::make_unique<BrowserOverlayRenderData>();
    std::unique_ptr<BrowserOverlayRenderData> backBuffer = std::make_unique<BrowserOverlayRenderData>();
    std::atomic<bool> hasNewFrame{ false };
    std::mutex swapMutex;
    std::mutex renderStateMutex;

    GLuint glTextureId = 0;
    int glTextureWidth = 0;
    int glTextureHeight = 0;
    std::array<GLuint, kBrowserOverlayUploadPboCount> uploadPbos{};
    size_t nextUploadPboIndex = 0;
    BrowserOverlayRenderData* lastUploadedRenderData = nullptr;
    BrowserOverlayRenderData* lastPublishedRenderData = nullptr;
    std::shared_ptr<const std::vector<unsigned char>> publishedPixels;
    uint64_t publishedGeneration = 0;
    bool filterInitialized = false;
    bool lastPixelatedScaling = false;
};

struct BrowserOverlayPendingGlCleanup {
    GLuint textureId = 0;
    std::array<GLuint, kBrowserOverlayUploadPboCount> uploadPbos{};
};

struct BrowserOverlayEncodedFrame {
    std::string overlayId;
    uint64_t sequence = 0;
    MacBrowserView::Snapshot snapshot;
};

struct PixelStoreStateGuard {
    GLint unpackAlignment = 4;
    GLint unpackRowLength = 0;
    GLint unpackSkipRows = 0;
    GLint unpackSkipPixels = 0;

    PixelStoreStateGuard() {
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackAlignment);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpackRowLength);
        glGetIntegerv(GL_UNPACK_SKIP_ROWS, &unpackSkipRows);
        glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &unpackSkipPixels);
    }

    ~PixelStoreStateGuard() {
        glPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlignment);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, unpackRowLength);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, unpackSkipRows);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, unpackSkipPixels);
    }
};

std::map<std::string, std::shared_ptr<BrowserOverlayCacheEntry>> g_browserOverlayCache;
std::mutex g_browserOverlayCacheMutex;

std::vector<BrowserOverlayPendingGlCleanup> g_browserOverlayPendingGlCleanup;
std::mutex g_browserOverlayPendingGlCleanupMutex;

std::atomic<bool> g_stopBrowserOverlayThread{ false };
std::thread g_browserOverlayThread;
std::thread g_browserOverlayDecodeThread;

std::map<std::string, BrowserOverlayEncodedFrame> g_browserOverlayPendingDecodeFrames;
std::mutex g_browserOverlayDecodeMutex;
std::condition_variable g_browserOverlayDecodeCv;

std::vector<std::function<void()>> g_browserOverlayPendingEvents;
std::mutex g_browserOverlayPendingEventsMutex;

void LogBrowserOverlayMessage(const std::string& message) {
    LogCategory("browser_overlay", message);
}

void PostBrowserOverlayEvent(std::function<void()> event) {
    std::lock_guard<std::mutex> lock(g_browserOverlayPendingEventsMutex);
    g_browserOverlayPendingEvents.push_back(std::move(event));
}

void PumpBrowserOverlayMessages() {
    std::vector<std::function<void()>> events;
    {
        std::lock_guard<std::mutex> lock(g_browserOverlayPendingEventsMutex);
        events.swap(g_browserOverlayPendingEvents);
    }
    for (auto& event : events) {
        event();
    }
}

void AppendJavaScriptHexEscape(std::wstring& output, wchar_t value) {
    static constexpr wchar_t kHexDigits[] = L"0123456789ABCDEF";
    output += L"\\u";
    output.push_back(kHexDigits[(value >> 12) & 0xF]);
    output.push_back(kHexDigits[(value >> 8) & 0xF]);
    output.push_back(kHexDigits[(value >> 4) & 0xF]);
    output.push_back(kHexDigits[value & 0xF]);
}

std::wstring EscapeForJavaScriptStringLiteral(const std::wstring& input) {
    std::wstring escaped;
    escaped.reserve(input.size() + 16);
    escaped.push_back(L'\'');

    for (wchar_t ch : input) {
        switch (ch) {
        case L'\\':
            escaped += L"\\\\";
            break;
        case L'\'':
            escaped += L"\\'";
            break;
        case L'\r':
            escaped += L"\\r";
            break;
        case L'\n':
            escaped += L"\\n";
            break;
        case L'\t':
            escaped += L"\\t";
            break;
        default:
            if (ch < 0x20 || ch == 0x2028 || ch == 0x2029) {
                AppendJavaScriptHexEscape(escaped, ch);
            } else {
                escaped.push_back(ch);
            }
            break;
        }
    }

    escaped.push_back(L'\'');
    return escaped;
}

std::wstring BuildBrowserOverlayInjectedCss(const BrowserOverlayCacheEntry& entry) {
    std::wstring css;

    if (entry.transparentBackground) {
        css += L"html, body { background: transparent !important; }\n";
    }

    if (!entry.customCss.empty()) {
        css += Utf8ToWide(entry.customCss);
    }

    return css;
}

void ApplyBrowserOverlayInjectedStyles(BrowserOverlayCacheEntry& entry) {
    if (!entry.viewCreated) {
        return;
    }

    const std::wstring cssLiteral = EscapeForJavaScriptStringLiteral(BuildBrowserOverlayInjectedCss(entry));
    const std::wstring script =
        LR"JS((() => {
    try {
        const styleId = '__toolscreen_browser_overlay_style';
        const cssText = )JS" +
        cssLiteral +
        LR"JS(;
        let style = document.getElementById(styleId);
        if (!cssText) {
            if (style) {
                style.remove();
            }
            return;
        }

        const root = document.head || document.documentElement || document.body;
        if (!root) {
            return;
        }

        if (!style) {
            style = document.createElement('style');
            style.id = styleId;
            root.appendChild(style);
        }

        style.textContent = cssText;
    } catch (e) {
    }
})();)JS";

    MacBrowserView::ExecuteScript(entry.name, WideToUtf8(script));
}

void UpdateBrowserOverlayBuffer(BrowserOverlayCacheEntry& entry, const unsigned char* pixels, int width, int height) {
    if (!pixels || width <= 0 || height <= 0) {
        return;
    }

    if (entry.writeBuffer->width != width || entry.writeBuffer->height != height) {
        if (entry.writeBuffer->pixelData) {
            delete[] entry.writeBuffer->pixelData;
            entry.writeBuffer->pixelData = nullptr;
        }
        entry.writeBuffer->width = width;
        entry.writeBuffer->height = height;
        const size_t bufferSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
        if (bufferSize > 0 && bufferSize < kBrowserOverlayMaxFrameBytes) {
            entry.writeBuffer->pixelData = new unsigned char[bufferSize];
        }
    }

    if (!entry.writeBuffer->pixelData) {
        return;
    }

    const size_t copySize = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    memcpy(entry.writeBuffer->pixelData, pixels, copySize);

    {
        std::lock_guard<std::mutex> lock(entry.swapMutex);
        entry.writeBuffer.swap(entry.readyBuffer);
        entry.hasNewFrame.store(true, std::memory_order_release);
    }
}

void EnqueueBrowserOverlayDecode(BrowserOverlayEncodedFrame&& frame) {
    {
        std::lock_guard<std::mutex> lock(g_browserOverlayDecodeMutex);
        auto& slot = g_browserOverlayPendingDecodeFrames[frame.overlayId];
        slot = std::move(frame);
    }
    g_browserOverlayDecodeCv.notify_one();
}

void QueueBrowserOverlayGlCleanup(BrowserOverlayCacheEntry& entry) {
    std::lock_guard<std::mutex> renderLock(entry.renderStateMutex);

    BrowserOverlayPendingGlCleanup cleanup{};
    cleanup.textureId = entry.glTextureId;
    cleanup.uploadPbos = entry.uploadPbos;

    entry.glTextureId = 0;
    entry.glTextureWidth = 0;
    entry.glTextureHeight = 0;
    entry.uploadPbos.fill(0);
    entry.nextUploadPboIndex = 0;
    entry.lastUploadedRenderData = nullptr;
    entry.filterInitialized = false;
    entry.lastPixelatedScaling = false;

    bool hasResources = cleanup.textureId != 0;
    if (!hasResources) {
        for (GLuint pboId : cleanup.uploadPbos) {
            if (pboId != 0) {
                hasResources = true;
                break;
            }
        }
    }

    if (!hasResources) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_browserOverlayPendingGlCleanupMutex);
    g_browserOverlayPendingGlCleanup.push_back(cleanup);
}

void FlushBrowserOverlayGlCleanup() {
    if (!wglGetCurrentContext()) {
        return;
    }

    std::vector<BrowserOverlayPendingGlCleanup> pending;
    {
        std::lock_guard<std::mutex> lock(g_browserOverlayPendingGlCleanupMutex);
        if (g_browserOverlayPendingGlCleanup.empty()) {
            return;
        }
        pending.swap(g_browserOverlayPendingGlCleanup);
    }

    for (const BrowserOverlayPendingGlCleanup& cleanup : pending) {
        if (cleanup.textureId != 0) {
            glDeleteTextures(1, &cleanup.textureId);
        }

        GLuint pboIds[kBrowserOverlayUploadPboCount] = {};
        GLsizei pboCount = 0;
        for (GLuint pboId : cleanup.uploadPbos) {
            if (pboId != 0) {
                pboIds[pboCount++] = pboId;
            }
        }
        if (pboCount > 0) {
            glDeleteBuffers(pboCount, pboIds);
        }
    }
}

void UploadBrowserOverlayTexture(BrowserOverlayCacheEntry& entry, const BrowserOverlayRenderData& renderData) {
    PixelStoreStateGuard pixelStoreGuard;
    if (entry.glTextureId == 0) {
        glGenTextures(1, &entry.glTextureId);
        BindTextureDirect(GL_TEXTURE_2D, entry.glTextureId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        entry.filterInitialized = false;
    }

    BindTextureDirect(GL_TEXTURE_2D, entry.glTextureId);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    const size_t bufferSize = static_cast<size_t>(renderData.width) * static_cast<size_t>(renderData.height) * 4;
    bool uploadedWithPbo = false;
    if (bufferSize > 0 && bufferSize < kBrowserOverlayMaxFrameBytes) {
        const size_t pboIndex = entry.nextUploadPboIndex % kBrowserOverlayUploadPboCount;
        entry.nextUploadPboIndex = (pboIndex + 1) % kBrowserOverlayUploadPboCount;

        GLuint& pboId = entry.uploadPbos[pboIndex];
        if (pboId == 0) {
            glGenBuffers(1, &pboId);
        }

        if (pboId != 0) {
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pboId);
            glBufferData(GL_PIXEL_UNPACK_BUFFER, static_cast<GLsizeiptr>(bufferSize), nullptr, GL_STREAM_DRAW);

            GLbitfield mapFlags = GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT;
#ifdef GL_MAP_UNSYNCHRONIZED_BIT
            mapFlags |= GL_MAP_UNSYNCHRONIZED_BIT;
#endif
            void* mapped = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, static_cast<GLsizeiptr>(bufferSize), mapFlags);
            if (mapped) {
                memcpy(mapped, renderData.pixelData, bufferSize);
                const GLboolean unmapped = glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
                if (unmapped == GL_TRUE) {
                    if (entry.glTextureWidth != renderData.width || entry.glTextureHeight != renderData.height) {
                        entry.glTextureWidth = renderData.width;
                        entry.glTextureHeight = renderData.height;
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, renderData.width, renderData.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                     nullptr);
                    } else {
                        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, renderData.width, renderData.height, GL_RGBA, GL_UNSIGNED_BYTE,
                                        nullptr);
                    }
                    uploadedWithPbo = true;
                }
            }

            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        }
    }

    if (!uploadedWithPbo) {
        if (entry.glTextureWidth != renderData.width || entry.glTextureHeight != renderData.height) {
            entry.glTextureWidth = renderData.width;
            entry.glTextureHeight = renderData.height;
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, renderData.width, renderData.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         renderData.pixelData);
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, renderData.width, renderData.height, GL_RGBA, GL_UNSIGNED_BYTE,
                            renderData.pixelData);
        }
    }

    entry.lastUploadedRenderData = const_cast<BrowserOverlayRenderData*>(&renderData);
}

void NavigateBrowserOverlay(BrowserOverlayCacheEntry& entry) {
    if (!entry.viewCreated) {
        return;
    }

    entry.hasSeenContentLoading = false;
    entry.pendingReload = false;
    MacBrowserView::Navigate(entry.name, entry.url);
    entry.lastReloadTime = std::chrono::steady_clock::now();
}

void UpdateBrowserOverlayBounds(BrowserOverlayCacheEntry& entry) {
    const int width = (std::max)(1, entry.browserWidth);
    const int height = (std::max)(1, entry.browserHeight);

    if (entry.viewCreated || entry.controllerCreationPending) {
        MacBrowserView::SetSize(entry.name, width, height);
    }
}

void ApplyBrowserOverlayControllerSettings(BrowserOverlayCacheEntry& entry) {
    if (!entry.viewCreated) {
        return;
    }

    MacBrowserView::SetTransparentBackground(entry.name, entry.transparentBackground);
    MacBrowserView::SetMuted(entry.name, entry.muteAudio);
}

void DestroyBrowserOverlayEntry(BrowserOverlayCacheEntry& entry) {
    QueueBrowserOverlayGlCleanup(entry);
    if (entry.viewCreated || entry.controllerCreationPending) {
        MacBrowserView::Destroy(entry.name);
    }
    entry.viewCreated = false;
    entry.controllerCreationPending = false;
    ++entry.viewGeneration;
}

void RequestBrowserOverlayReload(BrowserOverlayCacheEntry& entry) {
    if (!entry.viewCreated) {
        return;
    }

    entry.pendingReload = false;
    entry.hasSeenContentLoading = false;
    MacBrowserView::Reload(entry.name);
    entry.lastReloadTime = std::chrono::steady_clock::now();
}

void ConfigureBrowserOverlayController(BrowserOverlayCacheEntry& entry) {
    if (!entry.viewCreated) {
        return;
    }

    UpdateBrowserOverlayBounds(entry);
    ApplyBrowserOverlayControllerSettings(entry);
    NavigateBrowserOverlay(entry);
}

void OnBrowserOverlayContentLoading(const std::string& overlayId) {
    std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
    auto it = g_browserOverlayCache.find(overlayId);
    if (it != g_browserOverlayCache.end() && it->second) {
        it->second->hasSeenContentLoading = true;
        ApplyBrowserOverlayInjectedStyles(*it->second);
    }
}

void OnBrowserOverlayNavigationCompleted(const std::string& overlayId, bool success, long errorCode) {
    std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
    auto it = g_browserOverlayCache.find(overlayId);
    if (it == g_browserOverlayCache.end() || !it->second) {
        return;
    }

    it->second->lastReloadTime = std::chrono::steady_clock::now();
    if (!success) {
        LogBrowserOverlayMessage("[BrowserOverlay] Navigation failed for '" + overlayId + "' with status " + std::to_string(errorCode));
    } else {
        ApplyBrowserOverlayInjectedStyles(*it->second);
    }
}

void OnBrowserOverlayProcessFailed(const std::string& overlayId) {
    {
        std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
        auto it = g_browserOverlayCache.find(overlayId);
        if (it != g_browserOverlayCache.end() && it->second) {
            it->second->pendingReload = true;
            it->second->captureInFlight = false;
        }
    }

    LogBrowserOverlayMessage("[BrowserOverlay] Process failure for '" + overlayId + "'");
}

void OnBrowserOverlayViewCreated(const std::string& overlayId, uint64_t generation, bool created) {
    std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
    auto it = g_browserOverlayCache.find(overlayId);
    if (it == g_browserOverlayCache.end() || !it->second) {
        return;
    }

    BrowserOverlayCacheEntry& entry = *it->second;
    if (entry.viewGeneration != generation) {
        return;
    }

    entry.controllerCreationPending = false;
    if (!created) {
        entry.controllerCreationFailed = true;
        LogBrowserOverlayMessage("[BrowserOverlay] Failed to create web view for '" + overlayId + "'");
        return;
    }

    entry.viewCreated = true;
    entry.controllerCreationFailed = false;
    ConfigureBrowserOverlayController(entry);
}

void BeginBrowserOverlayControllerCreation(const std::string& overlayId, BrowserOverlayCacheEntry& entry) {
    if (entry.viewCreated || entry.controllerCreationPending || entry.controllerCreationFailed) {
        return;
    }

    entry.controllerCreationPending = true;
    const uint64_t generation = ++entry.viewGeneration;

    MacBrowserView::Callbacks callbacks;
    callbacks.contentLoading = [overlayId]() {
        PostBrowserOverlayEvent([overlayId]() { OnBrowserOverlayContentLoading(overlayId); });
    };
    callbacks.navigationCompleted = [overlayId](bool success, long errorCode) {
        PostBrowserOverlayEvent([overlayId, success, errorCode]() { OnBrowserOverlayNavigationCompleted(overlayId, success, errorCode); });
    };
    callbacks.processFailed = [overlayId]() {
        PostBrowserOverlayEvent([overlayId]() { OnBrowserOverlayProcessFailed(overlayId); });
    };

    MacBrowserView::Create(overlayId, entry.browserWidth, entry.browserHeight, std::move(callbacks),
                           [overlayId, generation](bool created) {
                               PostBrowserOverlayEvent(
                                   [overlayId, generation, created]() { OnBrowserOverlayViewCreated(overlayId, generation, created); });
                           });
}

void SyncBrowserOverlayEntry(const BrowserOverlayConfig& config) {
    std::shared_ptr<BrowserOverlayCacheEntry> entryPtr;
    bool urlChanged = false;
    bool cssChanged = false;
    bool sizeChanged = false;
    bool transparencyChanged = false;
    const bool transparentModeEnabled = config.transparentBackground;

    {
        std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
        auto& slot = g_browserOverlayCache[config.name];
        if (!slot) {
            slot = std::make_shared<BrowserOverlayCacheEntry>();
            slot->name = config.name;
            slot->url = config.url;
            slot->customCss = config.customCss;
            slot->browserWidth = (std::max)(1, config.browserWidth);
            slot->browserHeight = (std::max)(1, config.browserHeight);
            slot->fps = (std::max)(1, config.fps);
            slot->transparentBackground = transparentModeEnabled;
            slot->muteAudio = config.muteAudio;
            slot->hardwareAcceleration = config.hardwareAcceleration;
            slot->allowSystemMediaKeys = config.allowSystemMediaKeys;
            slot->reloadOnUpdate = config.reloadOnUpdate;
            slot->reloadInterval = (std::max)(0, config.reloadInterval);
        } else {
            urlChanged = slot->url != config.url;
            cssChanged = slot->customCss != config.customCss;
            sizeChanged = slot->browserWidth != config.browserWidth || slot->browserHeight != config.browserHeight;
            transparencyChanged = slot->transparentBackground != transparentModeEnabled;
            slot->url = config.url;
            slot->customCss = config.customCss;
            slot->browserWidth = (std::max)(1, config.browserWidth);
            slot->browserHeight = (std::max)(1, config.browserHeight);
            slot->fps = (std::max)(1, config.fps);
            slot->transparentBackground = transparentModeEnabled;
            slot->muteAudio = config.muteAudio;
            slot->hardwareAcceleration = config.hardwareAcceleration;
            slot->allowSystemMediaKeys = config.allowSystemMediaKeys;
            slot->reloadOnUpdate = config.reloadOnUpdate;
            slot->reloadInterval = (std::max)(0, config.reloadInterval);
            slot->markedForRemoval = false;
        }
        entryPtr = slot;
    }

    if (!entryPtr) {
        return;
    }

    UpdateBrowserOverlayBounds(*entryPtr);

    // WebKit has no counterpart to the WebView2 environment switches behind
    // allowSystemMediaKeys and hardwareAcceleration, so changing them does not
    // recreate the view.
    if (!entryPtr->viewCreated) {
        entryPtr->controllerCreationFailed = false;
        BeginBrowserOverlayControllerCreation(config.name, *entryPtr);
        return;
    }

    if (transparencyChanged || cssChanged) {
        ApplyBrowserOverlayInjectedStyles(*entryPtr);
    }

    ApplyBrowserOverlayControllerSettings(*entryPtr);

    if (urlChanged) {
        NavigateBrowserOverlay(*entryPtr);
        return;
    }

    if (sizeChanged && entryPtr->reloadOnUpdate) {
        entryPtr->pendingReload = true;
    }
}

bool StageBrowserOverlayTestFrameInternal(const BrowserOverlayConfig& config, const std::vector<unsigned char>& rgbaPixels, int width,
                                         int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }

    const size_t expectedByteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    if (rgbaPixels.size() != expectedByteCount) {
        return false;
    }

    std::shared_ptr<BrowserOverlayCacheEntry> entry;
    {
        std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
        auto& slot = g_browserOverlayCache[config.name];
        if (!slot) {
            slot = std::make_shared<BrowserOverlayCacheEntry>();
        }

        entry = slot;
        entry->name = config.name;
        entry->url = config.url;
        entry->customCss = config.customCss;
        entry->browserWidth = (std::max)(1, config.browserWidth);
        entry->browserHeight = (std::max)(1, config.browserHeight);
        entry->fps = (std::max)(1, config.fps);
        entry->transparentBackground = config.transparentBackground;
        entry->muteAudio = config.muteAudio;
        entry->allowSystemMediaKeys = config.allowSystemMediaKeys;
        entry->reloadOnUpdate = config.reloadOnUpdate;
        entry->reloadInterval = (std::max)(0, config.reloadInterval);
        entry->markedForRemoval = false;
    }

    std::lock_guard<std::mutex> swapLock(entry->swapMutex);
    entry->readyBuffer = std::make_unique<BrowserOverlayRenderData>();
    entry->readyBuffer->pixelData = new unsigned char[expectedByteCount];
    std::copy(rgbaPixels.begin(), rgbaPixels.end(), entry->readyBuffer->pixelData);
    entry->readyBuffer->width = width;
    entry->readyBuffer->height = height;
    entry->lastUploadedRenderData = nullptr;
    entry->hasNewFrame.store(true, std::memory_order_release);
    return true;
}

void ReconcileBrowserOverlays() {
    auto snapshot = GetConfigSnapshot();
    if (!snapshot) {
        return;
    }

    std::set<std::string> activeNames;
    for (const auto& config : snapshot->browserOverlays) {
        activeNames.insert(config.name);
        SyncBrowserOverlayEntry(config);
    }

    std::vector<std::shared_ptr<BrowserOverlayCacheEntry>> removedEntries;
    {
        std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
        for (auto it = g_browserOverlayCache.begin(); it != g_browserOverlayCache.end();) {
            if (it->second && (it->second->markedForRemoval || activeNames.find(it->first) == activeNames.end())) {
                removedEntries.push_back(std::move(it->second));
                it = g_browserOverlayCache.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (auto& removed : removedEntries) {
        if (removed) {
            DestroyBrowserOverlayEntry(*removed);
        }
    }
}

void OnBrowserOverlayCaptured(const std::string& overlayId, MacBrowserView::Snapshot snapshot) {
    BrowserOverlayEncodedFrame frame{};
    frame.overlayId = overlayId;
    frame.snapshot = std::move(snapshot);

    {
        std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
        auto it = g_browserOverlayCache.find(overlayId);
        if (it != g_browserOverlayCache.end() && it->second) {
            it->second->captureInFlight = false;
            if (frame.snapshot) {
                frame.sequence = ++it->second->latestQueuedDecodeSequence;
            }
        }
    }

    if (!frame.snapshot) {
        LogBrowserOverlayMessage("[BrowserOverlay] Snapshot failed for '" + overlayId + "'");
        return;
    }

    if (frame.sequence == 0) {
        return;
    }

    EnqueueBrowserOverlayDecode(std::move(frame));
}

void TryCaptureBrowserOverlay(const std::string& overlayId) {
    {
        std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
        auto it = g_browserOverlayCache.find(overlayId);
        if (it == g_browserOverlayCache.end() || !it->second) {
            return;
        }

        BrowserOverlayCacheEntry& entry = *it->second;
        if (!entry.viewCreated || !entry.hasSeenContentLoading || entry.captureInFlight) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        const int captureIntervalMs = 1000 / (std::max)(1, entry.fps);
        if (entry.lastCaptureTime.time_since_epoch().count() != 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - entry.lastCaptureTime).count();
            if (elapsed < captureIntervalMs) {
                if (entry.reloadInterval > 0) {
                    const auto reloadElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - entry.lastReloadTime).count();
                    if (reloadElapsed >= entry.reloadInterval) {
                        RequestBrowserOverlayReload(entry);
                    }
                } else if (entry.pendingReload) {
                    RequestBrowserOverlayReload(entry);
                }
                return;
            }
        }

        if (entry.reloadInterval > 0) {
            const auto reloadElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - entry.lastReloadTime).count();
            if (reloadElapsed >= entry.reloadInterval) {
                RequestBrowserOverlayReload(entry);
            }
        } else if (entry.pendingReload) {
            RequestBrowserOverlayReload(entry);
        }

        entry.captureInFlight = true;
        entry.lastCaptureTime = now;
    }

    // The snapshot completes on the main thread; only the capture bookkeeping
    // and the decode hand-off happen there, both under the cache lock.
    MacBrowserView::Capture(overlayId,
                            [overlayId](MacBrowserView::Snapshot snapshot) { OnBrowserOverlayCaptured(overlayId, std::move(snapshot)); });
}

void BrowserOverlayDecodeThreadFunc() {
    LogBrowserOverlayMessage("[BrowserOverlay] Decode thread started");

    std::vector<unsigned char> pixels;
    while (true) {
        BrowserOverlayEncodedFrame frame{};
        {
            std::unique_lock<std::mutex> lock(g_browserOverlayDecodeMutex);
            g_browserOverlayDecodeCv.wait(lock, [] {
                return g_stopBrowserOverlayThread.load(std::memory_order_acquire) || !g_browserOverlayPendingDecodeFrames.empty();
            });

            if (g_stopBrowserOverlayThread.load(std::memory_order_acquire) && g_browserOverlayPendingDecodeFrames.empty()) {
                break;
            }

            auto it = g_browserOverlayPendingDecodeFrames.begin();
            frame = std::move(it->second);
            g_browserOverlayPendingDecodeFrames.erase(it);
        }

        if (!frame.snapshot || frame.sequence == 0) {
            continue;
        }

        int width = 0;
        int height = 0;
        if (!MacBrowserView::DecodeSnapshot(frame.snapshot, pixels, width, height) || width <= 0 || height <= 0) {
            continue;
        }

        std::shared_ptr<BrowserOverlayCacheEntry> entry;
        {
            std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
            auto it = g_browserOverlayCache.find(frame.overlayId);
            if (it != g_browserOverlayCache.end() && it->second && it->second->latestQueuedDecodeSequence == frame.sequence) {
                entry = it->second;
            }
        }

        if (entry) {
            UpdateBrowserOverlayBuffer(*entry, pixels.data(), width, height);
        }
    }

    LogBrowserOverlayMessage("[BrowserOverlay] Decode thread stopped");
}

void BrowserOverlayThreadFunc() {
    LogBrowserOverlayMessage("[BrowserOverlay] Thread started");

    uint64_t lastConfigVersion = UINT64_MAX;
    std::vector<std::string> overlayIds;
    while (!g_stopBrowserOverlayThread.load(std::memory_order_acquire)) {
        overlayIds.clear();
        PumpBrowserOverlayMessages();

        const uint64_t currentVersion = g_configSnapshotVersion.load(std::memory_order_acquire);
        if (currentVersion != lastConfigVersion) {
            lastConfigVersion = currentVersion;
            ReconcileBrowserOverlays();
        }

        if (g_browserOverlaysVisible.load(std::memory_order_acquire)) {
            {
                std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
                overlayIds.reserve(g_browserOverlayCache.size());
                for (const auto& [overlayId, entry] : g_browserOverlayCache) {
                    if (entry && !entry->markedForRemoval) {
                        overlayIds.push_back(overlayId);
                    }
                }
            }

            for (const auto& overlayId : overlayIds) {
                TryCaptureBrowserOverlay(overlayId);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    std::vector<std::shared_ptr<BrowserOverlayCacheEntry>> removedEntries;
    {
        std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
        for (auto& [_, entry] : g_browserOverlayCache) {
            removedEntries.push_back(std::move(entry));
        }
        g_browserOverlayCache.clear();
    }

    for (auto& entry : removedEntries) {
        if (entry) {
            DestroyBrowserOverlayEntry(*entry);
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_browserOverlayPendingEventsMutex);
        g_browserOverlayPendingEvents.clear();
    }

    LogBrowserOverlayMessage("[BrowserOverlay] Thread stopped");
}

} // namespace

bool StageBrowserOverlayTestFrame(const BrowserOverlayConfig& config, const std::vector<unsigned char>& rgbaPixels, int width, int height) {
    return StageBrowserOverlayTestFrameInternal(config, rgbaPixels, width, height);
}

const BrowserOverlayConfig* FindBrowserOverlayConfig(const std::string& overlayId) {
    const auto& overlays = g_config.browserOverlays;
    for (const auto& overlay : overlays) {
        if (overlay.name == overlayId) {
            return &overlay;
        }
    }
    return nullptr;
}

const BrowserOverlayConfig* FindBrowserOverlayConfigIn(const std::string& overlayId, const Config& config) {
    const auto& overlays = config.browserOverlays;
    for (const auto& overlay : overlays) {
        if (overlay.name == overlayId) {
            return &overlay;
        }
    }
    return nullptr;
}

void RemoveBrowserOverlayFromCache(const std::string& overlayId) {
    std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
    auto it = g_browserOverlayCache.find(overlayId);
    if (it != g_browserOverlayCache.end() && it->second) {
        it->second->markedForRemoval = true;
    }
}

void RequestBrowserOverlayRefresh(const std::string& overlayId) {
    std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
    auto it = g_browserOverlayCache.find(overlayId);
    if (it != g_browserOverlayCache.end() && it->second) {
        it->second->pendingReload = true;
        it->second->lastCaptureTime = std::chrono::steady_clock::time_point{};
    }
}

void CleanupBrowserOverlayCache() {
    std::vector<std::shared_ptr<BrowserOverlayCacheEntry>> entries;
    {
        std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
        entries.reserve(g_browserOverlayCache.size());
        for (auto& [_, entry] : g_browserOverlayCache) {
            if (!entry) {
                continue;
            }
            entry->markedForRemoval = true;
            entries.push_back(entry);
        }
    }

    for (const auto& entry : entries) {
        if (entry) {
            QueueBrowserOverlayGlCleanup(*entry);
        }
    }

    FlushBrowserOverlayGlCleanup();

    {
        std::lock_guard<std::mutex> lock(g_browserOverlayDecodeMutex);
        g_browserOverlayPendingDecodeFrames.clear();
    }
}

void StartBrowserOverlayThread() {
    if (!g_browserOverlayThread.joinable()) {
        g_stopBrowserOverlayThread.store(false, std::memory_order_release);
        g_browserOverlayThread = std::thread(BrowserOverlayThreadFunc);
    }
    if (!g_browserOverlayDecodeThread.joinable()) {
        g_browserOverlayDecodeThread = std::thread(BrowserOverlayDecodeThreadFunc);
    }
}

void StopBrowserOverlayThread() {
    if (g_browserOverlayThread.joinable()) {
        g_stopBrowserOverlayThread.store(true, std::memory_order_release);
        g_browserOverlayDecodeCv.notify_all();
        g_browserOverlayThread.join();
    }
    if (g_browserOverlayDecodeThread.joinable()) {
        g_browserOverlayDecodeCv.notify_all();
        g_browserOverlayDecodeThread.join();
    }

    {
        std::lock_guard<std::mutex> lock(g_browserOverlayDecodeMutex);
        g_browserOverlayPendingDecodeFrames.clear();
    }
}

bool PrepareBrowserOverlayTexture(const BrowserOverlayConfig& config, BrowserOverlayTextureFrame& outFrame) {
    outFrame = {};

    FlushBrowserOverlayGlCleanup();

    std::shared_ptr<BrowserOverlayCacheEntry> entry;
    {
        std::lock_guard<std::mutex> cacheLock(g_browserOverlayCacheMutex);
        auto it = g_browserOverlayCache.find(config.name);
        if (it == g_browserOverlayCache.end() || !it->second) {
            return false;
        }
        entry = it->second;
    }

    if (!entry) {
        return false;
    }

    if (entry->hasNewFrame.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> swapLock(entry->swapMutex);
        if (entry->hasNewFrame.load(std::memory_order_relaxed)) {
            entry->readyBuffer.swap(entry->backBuffer);
            entry->hasNewFrame.store(false, std::memory_order_release);
        }
    }

    BrowserOverlayRenderData* renderData = entry->backBuffer.get();
    std::lock_guard<std::mutex> renderLock(entry->renderStateMutex);
    if (renderData && renderData->pixelData && renderData->width > 0 && renderData->height > 0) {
        if (renderData != entry->lastUploadedRenderData) {
            UploadBrowserOverlayTexture(*entry, *renderData);
        }
    }

    if (entry->glTextureId == 0) {
        return false;
    }

    BindTextureDirect(GL_TEXTURE_2D, entry->glTextureId);
    if (!entry->filterInitialized || entry->lastPixelatedScaling != config.pixelatedScaling) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, config.pixelatedScaling ? GL_NEAREST : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, config.pixelatedScaling ? GL_NEAREST : GL_LINEAR);
        entry->lastPixelatedScaling = config.pixelatedScaling;
        entry->filterInitialized = true;
    }

    outFrame.textureId = entry->glTextureId;
    outFrame.textureWidth = entry->glTextureWidth;
    outFrame.textureHeight = entry->glTextureHeight;
    return outFrame.textureId != 0 && outFrame.textureWidth > 0 && outFrame.textureHeight > 0;
}

bool AcquireBrowserOverlayPixelFrame(
    const BrowserOverlayConfig& config, BrowserOverlayPixelFrame& outFrame) {
    outFrame = {};
    std::shared_ptr<BrowserOverlayCacheEntry> entry;
    {
        std::lock_guard<std::mutex> cacheLock(g_browserOverlayCacheMutex);
        auto it = g_browserOverlayCache.find(config.name);
        if (it == g_browserOverlayCache.end() || !it->second) return false;
        entry = it->second;
    }

    if (entry->hasNewFrame.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> swapLock(entry->swapMutex);
        if (entry->hasNewFrame.load(std::memory_order_relaxed)) {
            entry->readyBuffer.swap(entry->backBuffer);
            entry->hasNewFrame.store(false, std::memory_order_release);
        }
    }

    std::lock_guard<std::mutex> renderLock(entry->renderStateMutex);
    BrowserOverlayRenderData* renderData = entry->backBuffer.get();
    if (!renderData || !renderData->pixelData || renderData->width <= 0 ||
        renderData->height <= 0) {
        return false;
    }
    if (renderData != entry->lastPublishedRenderData || !entry->publishedPixels) {
        const size_t byteCount =
            static_cast<size_t>(renderData->width) *
            static_cast<size_t>(renderData->height) * 4;
        auto pixels = std::make_shared<std::vector<unsigned char>>(
            renderData->pixelData, renderData->pixelData + byteCount);
        entry->publishedPixels = std::move(pixels);
        entry->lastPublishedRenderData = renderData;
        ++entry->publishedGeneration;
    }
    outFrame.pixels = entry->publishedPixels;
    outFrame.width = renderData->width;
    outFrame.height = renderData->height;
    entry->glTextureWidth = renderData->width;
    entry->glTextureHeight = renderData->height;
    outFrame.generation = entry->publishedGeneration;
    return static_cast<bool>(outFrame.pixels);
}
