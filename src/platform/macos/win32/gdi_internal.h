#pragma once

#include "platform/macos/win32/win32.h"

#include <cstdint>
#include <vector>

namespace MacGdi {

// HCURSOR / HICON object. Pixels are straight RGBA, top-down.
struct CursorObject {
    static constexpr uint32_t kMagic = 0x54534355; // 'TSCU'
    uint32_t magic = kMagic;
    int width = 0;
    int height = 0;
    int hotX = 0;
    int hotY = 0;
    bool icon = false;
    bool shared = false; // LoadCursor / LR_SHARED results are never destroyed
    std::vector<uint8_t> rgba;
    void* nativeCursor = nullptr; // NSCursor, created on first use
};

CursorObject* AsCursor(HCURSOR cursor);
// NSCursor for an HCURSOR (created on first use), or nullptr.
void* NativeCursor(HCURSOR cursor);
HCURSOR ArrowCursor();

} // namespace MacGdi
