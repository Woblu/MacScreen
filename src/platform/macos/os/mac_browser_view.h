#pragma once

// WKWebView instances keyed by overlay name. Every call may be made from any
// thread; the work is queued to the main thread in call order and callbacks run
// on the main thread, never inside the call that triggered them.

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace MacBrowserView {

struct Callbacks {
    std::function<void()> contentLoading;
    std::function<void(bool success, long errorCode)> navigationCompleted;
    std::function<void()> processFailed;
};

// Retained CGImage.
using Snapshot = std::shared_ptr<const void>;

// The page is laid out at width/height divided by the display scale, like
// WebView2 at the monitor's DPI, and snapshots come out at width x height pixels.
void Create(const std::string& id, int width, int height, Callbacks callbacks, std::function<void(bool created)> done);
void Destroy(const std::string& id);
void SetSize(const std::string& id, int width, int height);
void SetTransparentBackground(const std::string& id, bool transparent);
void SetMuted(const std::string& id, bool muted);
void Navigate(const std::string& id, const std::string& url);
void Reload(const std::string& id);
void ExecuteScript(const std::string& id, const std::string& script);
void Capture(const std::string& id, std::function<void(Snapshot snapshot)> done);

// Converts a snapshot to tightly packed RGBA with straight alpha, top row first.
bool DecodeSnapshot(const Snapshot& snapshot, std::vector<unsigned char>& rgba, int& width, int& height);

} // namespace MacBrowserView
