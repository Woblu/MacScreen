// In-memory GDI: device contexts, bitmaps and cursor/icon objects. Supports
// what Toolscreen needs to turn cursor files into textures and to read pixels.

#include "platform/macos/win32/gdi_internal.h"

#include "platform/macos/os/mac_os.h"
#include "platform/macos/os/mac_window_capture.h"
#include "platform/macos/win32/user32_internal.h"
#include "platform/macos/win32/win32_internal.h"

#include "third_party/stb_image.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <mutex>
#include <string>

namespace {

enum class GdiType : uint32_t { Bitmap = 1, Dc, Brush, Region };

struct GdiObject {
    static constexpr uint32_t kMagic = 0x54534744; // 'TSGD'
    uint32_t magic = kMagic;
    GdiType type;
    explicit GdiObject(GdiType t) : type(t) {}
    virtual ~GdiObject() { magic = 0; }
};

// Bitmaps keep either 32-bit BGRA pixels or 1-bit values (one byte per pixel).
struct BitmapObject : GdiObject {
    int width = 0;
    int height = 0;
    int bitsPerPixel = 32;
    std::vector<uint8_t> pixels; // top-down
    BitmapObject() : GdiObject(GdiType::Bitmap) {}
};

struct DcObject : GdiObject {
    HWND window = nullptr;
    HGDIOBJ selected = nullptr;
    bool windowDc = false;
    int rop2 = R2_COPYPEN;
    DcObject() : GdiObject(GdiType::Dc) {}
};

struct BrushObject : GdiObject {
    COLORREF color = 0;
    bool stock = false;
    bool hollow = false; // NULL_BRUSH paints nothing
    BrushObject() : GdiObject(GdiType::Brush) {}
};

struct RegionObject : GdiObject {
    RECT rect{};
    RegionObject() : GdiObject(GdiType::Region) {}
};

GdiObject* AsGdi(const void* h) {
    if (!h) return nullptr;
    GdiObject* obj = static_cast<GdiObject*>(const_cast<void*>(h));
    return obj->magic == GdiObject::kMagic ? obj : nullptr;
}

template <typename T> T* As(const void* h, GdiType type) {
    GdiObject* obj = AsGdi(h);
    return (obj && obj->type == type) ? static_cast<T*>(obj) : nullptr;
}

std::mutex g_windowDcMutex;
DcObject* g_screenDc = nullptr;

uint16_t Le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t Le32(const uint8_t* p) { return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24)); }

// Decodes one ICO/CUR image entry (PNG or DIB with AND mask) to RGBA.
bool DecodeIconImage(const uint8_t* data, size_t size, std::vector<uint8_t>& rgba, int& width, int& height) {
    if (size >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
        int w = 0, h = 0, channels = 0;
        unsigned char* pixels = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &channels, 4);
        if (!pixels) return false;
        rgba.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
        stbi_image_free(pixels);
        width = w;
        height = h;
        return true;
    }
    if (size < 40) return false;
    const uint32_t headerSize = Le32(data);
    const int32_t w = static_cast<int32_t>(Le32(data + 4));
    const int32_t fullHeight = static_cast<int32_t>(Le32(data + 8));
    const uint16_t bitCount = Le16(data + 14);
    const uint32_t colorsUsed = Le32(data + 32);
    if (w <= 0 || fullHeight == 0 || headerSize < 40 || headerSize > size) return false;
    if (bitCount != 1 && bitCount != 4 && bitCount != 8 && bitCount != 24 && bitCount != 32) return false;
    const int h = std::abs(fullHeight) / 2;
    if (h <= 0) return false;

    size_t paletteEntries = 0;
    if (bitCount <= 8) paletteEntries = colorsUsed ? colorsUsed : (1u << bitCount);
    if (paletteEntries > (size - headerSize) / 4) return false;
    const size_t xorOffset = headerSize + paletteEntries * 4;
    const size_t xorStride = ((static_cast<size_t>(w) * bitCount + 31) / 32) * 4;
    if (xorStride > (size - xorOffset) / static_cast<size_t>(h)) return false;
    const size_t andOffset = xorOffset + xorStride * static_cast<size_t>(h);
    const size_t andStride = ((static_cast<size_t>(w) + 31) / 32) * 4;
    const bool hasAnd = andStride <= (size - andOffset) / static_cast<size_t>(h);
    const uint8_t* palette = data + headerSize;
    const uint8_t* xorBits = data + xorOffset;
    const uint8_t* andBits = data + andOffset;

    rgba.assign(static_cast<size_t>(w) * h * 4, 0);
    bool anyAlpha = false;
    for (int y = 0; y < h; ++y) {
        const int srcRow = h - 1 - y; // DIBs are bottom-up
        const uint8_t* row = xorBits + xorStride * static_cast<size_t>(srcRow);
        for (int x = 0; x < w; ++x) {
            uint8_t r = 0, g = 0, b = 0, a = 255;
            switch (bitCount) {
            case 32:
                b = row[x * 4 + 0];
                g = row[x * 4 + 1];
                r = row[x * 4 + 2];
                a = row[x * 4 + 3];
                if (a) anyAlpha = true;
                break;
            case 24:
                b = row[x * 3 + 0];
                g = row[x * 3 + 1];
                r = row[x * 3 + 2];
                break;
            case 8:
            case 4:
            case 1: {
                const int bitsPerPixel = bitCount;
                const size_t bitIndex = static_cast<size_t>(x) * bitsPerPixel;
                const uint8_t byte = row[bitIndex / 8];
                const int shift = 8 - bitsPerPixel - static_cast<int>(bitIndex % 8);
                const uint32_t index = (byte >> shift) & ((1u << bitsPerPixel) - 1);
                if (index < paletteEntries) {
                    b = palette[index * 4 + 0];
                    g = palette[index * 4 + 1];
                    r = palette[index * 4 + 2];
                }
                break;
            }
            default: return false;
            }
            uint8_t* out = &rgba[(static_cast<size_t>(y) * w + x) * 4];
            out[0] = r;
            out[1] = g;
            out[2] = b;
            out[3] = a;
        }
    }
    if (bitCount != 32 || !anyAlpha) {
        for (int y = 0; y < h; ++y) {
            const uint8_t* row = hasAnd ? andBits + andStride * static_cast<size_t>(h - 1 - y) : nullptr;
            for (int x = 0; x < w; ++x) {
                const bool transparent = row && ((row[x / 8] >> (7 - (x % 8))) & 1);
                uint8_t* out = &rgba[(static_cast<size_t>(y) * w + x) * 4];
                if (transparent) {
                    // AND=1 with a non-black XOR pixel inverts the screen; show it as black.
                    const bool invert = out[0] || out[1] || out[2];
                    out[0] = out[1] = out[2] = 0;
                    out[3] = invert ? 255 : 0;
                } else {
                    out[3] = 255;
                }
            }
        }
    }
    width = w;
    height = h;
    return true;
}

std::vector<uint8_t> Resize(const std::vector<uint8_t>& src, int sw, int sh, int dw, int dh) {
    if (sw == dw && sh == dh) return src;
    std::vector<uint8_t> out(static_cast<size_t>(dw) * dh * 4);
    for (int y = 0; y < dh; ++y) {
        const float fy = (y + 0.5f) * sh / dh - 0.5f;
        const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, sh - 1);
        const int y1 = std::min(y0 + 1, sh - 1);
        const float ty = std::clamp(fy - y0, 0.0f, 1.0f);
        for (int x = 0; x < dw; ++x) {
            const float fx = (x + 0.5f) * sw / dw - 0.5f;
            const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, sw - 1);
            const int x1 = std::min(x0 + 1, sw - 1);
            const float tx = std::clamp(fx - x0, 0.0f, 1.0f);
            // Premultiplied bilinear filtering.
            float acc[4] = { 0, 0, 0, 0 };
            const int xs[2] = { x0, x1 };
            const int ys[2] = { y0, y1 };
            const float wx[2] = { 1 - tx, tx };
            const float wy[2] = { 1 - ty, ty };
            for (int j = 0; j < 2; ++j) {
                for (int i = 0; i < 2; ++i) {
                    const uint8_t* p = &src[(static_cast<size_t>(ys[j]) * sw + xs[i]) * 4];
                    const float w = wx[i] * wy[j];
                    const float a = p[3] / 255.0f;
                    acc[0] += p[0] * a * w;
                    acc[1] += p[1] * a * w;
                    acc[2] += p[2] * a * w;
                    acc[3] += a * w;
                }
            }
            uint8_t* o = &out[(static_cast<size_t>(y) * dw + x) * 4];
            if (acc[3] > 0.0f) {
                o[0] = static_cast<uint8_t>(std::clamp(acc[0] / acc[3], 0.0f, 255.0f));
                o[1] = static_cast<uint8_t>(std::clamp(acc[1] / acc[3], 0.0f, 255.0f));
                o[2] = static_cast<uint8_t>(std::clamp(acc[2] / acc[3], 0.0f, 255.0f));
            }
            o[3] = static_cast<uint8_t>(std::clamp(acc[3] * 255.0f, 0.0f, 255.0f));
        }
    }
    return out;
}

MacGdi::CursorObject* LoadIconFile(const std::string& path, bool isCursor, int wantW, int wantH) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return nullptr;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.size() < 6 || Le16(bytes.data()) != 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    const uint16_t type = Le16(bytes.data() + 2);
    const uint16_t count = Le16(bytes.data() + 4);
    if ((type != 1 && type != 2) || count == 0 || bytes.size() < 6u + 16u * count) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    // Pick the entry closest to the requested size, preferring more colors.
    int best = -1;
    long bestScore = 0;
    for (int i = 0; i < count; ++i) {
        const uint8_t* e = bytes.data() + 6 + 16 * i;
        const int w = e[0] ? e[0] : 256;
        const int h = e[1] ? e[1] : 256;
        const int bits = (type == 1) ? Le16(e + 6) : 32;
        const long sizeDiff = std::labs(static_cast<long>(w) - wantW) + std::labs(static_cast<long>(h) - wantH);
        const long score = sizeDiff * 64 - bits;
        if (best < 0 || score < bestScore) {
            best = i;
            bestScore = score;
        }
    }
    const uint8_t* entry = bytes.data() + 6 + 16 * best;
    const uint32_t imageSize = Le32(entry + 8);
    const uint32_t imageOffset = Le32(entry + 12);
    if (imageOffset >= bytes.size() || imageSize == 0 || imageSize > bytes.size() - imageOffset) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    if (!DecodeIconImage(bytes.data() + imageOffset, imageSize, rgba, w, h)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    auto* cursor = new MacGdi::CursorObject();
    cursor->icon = !isCursor;
    int hotX = (type == 2) ? Le16(entry + 4) : w / 2;
    int hotY = (type == 2) ? Le16(entry + 6) : h / 2;
    const int dw = wantW > 0 ? wantW : w;
    const int dh = wantH > 0 ? wantH : h;
    if (dw != w || dh != h) {
        rgba = Resize(rgba, w, h, dw, dh);
        hotX = hotX * dw / w;
        hotY = hotY * dh / h;
        w = dw;
        h = dh;
    }
    cursor->rgba = std::move(rgba);
    cursor->width = w;
    cursor->height = h;
    cursor->hotX = hotX;
    cursor->hotY = hotY;
    return cursor;
}

MacGdi::CursorObject* CursorFromSystem(MacOs::SystemCursorKind kind) {
    void* ns = MacOs::SystemCursor(kind);
    if (!ns) return nullptr;
    auto* cursor = new MacGdi::CursorObject();
    cursor->shared = true;
    cursor->nativeCursor = ns;
    if (!MacOs::CursorImage(ns, cursor->rgba, cursor->width, cursor->height, cursor->hotX, cursor->hotY)) {
        cursor->width = cursor->height = 0;
    }
    return cursor;
}

BitmapObject* MakeBitmap(int w, int h, int bpp) {
    auto* b = new BitmapObject();
    b->width = w;
    b->height = h;
    b->bitsPerPixel = bpp;
    b->pixels.assign(static_cast<size_t>(w) * h * (bpp == 32 ? 4 : 1), 0);
    return b;
}

} // namespace

namespace MacGdi {

CursorObject* AsCursor(HCURSOR cursor) {
    if (!cursor) return nullptr;
    CursorObject* c = reinterpret_cast<CursorObject*>(cursor);
    return c->magic == CursorObject::kMagic ? c : nullptr;
}

void* NativeCursor(HCURSOR handle) {
    CursorObject* c = AsCursor(handle);
    if (!c) return nullptr;
    if (!c->nativeCursor && c->width > 0 && !c->rgba.empty()) {
        c->nativeCursor = MacOs::CreateCursor(c->rgba.data(), c->width, c->height, c->hotX, c->hotY);
    }
    return c->nativeCursor;
}

HCURSOR ArrowCursor() {
    static HCURSOR arrow = reinterpret_cast<HCURSOR>(CursorFromSystem(MacOs::SystemCursorKind::Arrow));
    return arrow;
}

} // namespace MacGdi

extern "C" {

HCURSOR LoadCursorW(HINSTANCE, LPCWSTR name) {
    if (!IS_INTRESOURCE(name)) return nullptr;
    static std::mutex mutex;
    static std::vector<std::pair<uintptr_t, HCURSOR>> cache;
    const uintptr_t id = reinterpret_cast<uintptr_t>(name);
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& entry : cache) {
        if (entry.first == id) return entry.second;
    }
    MacOs::SystemCursorKind kind = MacOs::SystemCursorKind::Arrow;
    switch (id) {
    case 32513: kind = MacOs::SystemCursorKind::IBeam; break;
    case 32514: kind = MacOs::SystemCursorKind::Wait; break;
    case 32515: kind = MacOs::SystemCursorKind::Cross; break;
    case 32649: kind = MacOs::SystemCursorKind::Hand; break;
    case 32644: kind = MacOs::SystemCursorKind::ResizeEW; break;
    case 32645: kind = MacOs::SystemCursorKind::ResizeNS; break;
    case 32642: kind = MacOs::SystemCursorKind::ResizeNWSE; break;
    case 32643: kind = MacOs::SystemCursorKind::ResizeNESW; break;
    case 32646: kind = MacOs::SystemCursorKind::ResizeAll; break;
    case 32648: kind = MacOs::SystemCursorKind::NotAllowed; break;
    default: break;
    }
    HCURSOR cursor = reinterpret_cast<HCURSOR>(CursorFromSystem(kind));
    cache.emplace_back(id, cursor);
    return cursor;
}

HANDLE LoadImageW(HINSTANCE, LPCWSTR name, UINT type, int cx, int cy, UINT flags) {
    if (!(flags & LR_LOADFROMFILE) || !name || IS_INTRESOURCE(name)) {
        if (IS_INTRESOURCE(name) && type == IMAGE_CURSOR) return LoadCursorW(nullptr, name);
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    std::string path = MacWin32::WideToUtf8(name);
    std::replace(path.begin(), path.end(), '\\', '/');
    const int systemSize = MacOs::SystemCursorSizePixels();
    const int w = cx > 0 ? cx : ((flags & LR_DEFAULTSIZE) ? systemSize : 0);
    const int h = cy > 0 ? cy : ((flags & LR_DEFAULTSIZE) ? systemSize : 0);
    if (type == IMAGE_CURSOR || type == IMAGE_ICON) return LoadIconFile(path, type == IMAGE_CURSOR, w, h);
    SetLastError(ERROR_INVALID_PARAMETER);
    return nullptr;
}

HANDLE CopyImage(HANDLE image, UINT, int cx, int cy, UINT flags) {
    MacGdi::CursorObject* src = MacGdi::AsCursor(static_cast<HCURSOR>(image));
    if (!src || src->width <= 0) return nullptr;
    const int w = cx > 0 ? cx : src->width;
    const int h = cy > 0 ? cy : src->height;
    if ((flags & LR_COPYRETURNORG) && w == src->width && h == src->height) return image;
    auto* copy = new MacGdi::CursorObject();
    copy->icon = src->icon;
    copy->rgba = Resize(src->rgba, src->width, src->height, w, h);
    copy->width = w;
    copy->height = h;
    copy->hotX = src->hotX * w / src->width;
    copy->hotY = src->hotY * h / src->height;
    return copy;
}

static BOOL DestroyCursorObject(HCURSOR cursor) {
    MacGdi::CursorObject* c = MacGdi::AsCursor(cursor);
    if (!c) return FALSE;
    if (c->shared) return TRUE;
    if (c->nativeCursor) MacOs::ReleaseCursor(c->nativeCursor);
    c->magic = 0;
    delete c;
    return TRUE;
}

BOOL DestroyCursor(HCURSOR cursor) { return DestroyCursorObject(cursor); }
BOOL DestroyIcon(HICON icon) { return DestroyCursorObject(reinterpret_cast<HCURSOR>(icon)); }

static BOOL FillIconInfo(HICON icon, BOOL& fIcon, DWORD& hotX, DWORD& hotY, HBITMAP& mask, HBITMAP& color) {
    MacGdi::CursorObject* c = MacGdi::AsCursor(reinterpret_cast<HCURSOR>(icon));
    if (!c || c->width <= 0 || c->height <= 0) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    fIcon = c->icon ? TRUE : FALSE;
    hotX = static_cast<DWORD>(c->hotX);
    hotY = static_cast<DWORD>(c->hotY);
    BitmapObject* colorBmp = MakeBitmap(c->width, c->height, 32);
    BitmapObject* maskBmp = MakeBitmap(c->width, c->height, 1);
    for (size_t i = 0, p = 0; i + 3 < c->rgba.size(); i += 4, ++p) {
        colorBmp->pixels[i + 0] = c->rgba[i + 2];
        colorBmp->pixels[i + 1] = c->rgba[i + 1];
        colorBmp->pixels[i + 2] = c->rgba[i + 0];
        colorBmp->pixels[i + 3] = c->rgba[i + 3];
        maskBmp->pixels[p] = c->rgba[i + 3] == 0 ? 1 : 0;
    }
    color = reinterpret_cast<HBITMAP>(colorBmp);
    mask = reinterpret_cast<HBITMAP>(maskBmp);
    return TRUE;
}

BOOL GetIconInfo(HICON icon, PICONINFO info) {
    if (!info) return FALSE;
    return FillIconInfo(icon, info->fIcon, info->xHotspot, info->yHotspot, info->hbmMask, info->hbmColor);
}

BOOL GetIconInfoExW(HICON icon, PICONINFOEXW info) {
    if (!info) return FALSE;
    info->wResID = 0;
    info->szModName[0] = L'\0';
    info->szResName[0] = L'\0';
    return FillIconInfo(icon, info->fIcon, info->xHotspot, info->yHotspot, info->hbmMask, info->hbmColor);
}

HDC GetDC(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_windowDcMutex);
    if (!hwnd) {
        if (!g_screenDc) {
            g_screenDc = new DcObject();
            g_screenDc->windowDc = true;
        }
        return reinterpret_cast<HDC>(g_screenDc);
    }
    static std::vector<DcObject*> windowDcs;
    for (DcObject* dc : windowDcs) {
        if (dc->window == hwnd) return reinterpret_cast<HDC>(dc);
    }
    DcObject* dc = new DcObject();
    dc->window = hwnd;
    dc->windowDc = true;
    windowDcs.push_back(dc);
    return reinterpret_cast<HDC>(dc);
}

int ReleaseDC(HWND, HDC) { return 1; }

HDC CreateCompatibleDC(HDC) { return reinterpret_cast<HDC>(new DcObject()); }

BOOL DeleteDC(HDC hdc) {
    DcObject* dc = As<DcObject>(hdc, GdiType::Dc);
    if (!dc || dc->windowDc) return FALSE;
    delete dc;
    return TRUE;
}

HWND WindowFromDC(HDC hdc) {
    DcObject* dc = As<DcObject>(hdc, GdiType::Dc);
    return dc ? dc->window : nullptr;
}

HBITMAP CreateCompatibleBitmap(HDC, int width, int height) {
    if (width <= 0 || height <= 0) return nullptr;
    return reinterpret_cast<HBITMAP>(MakeBitmap(width, height, 32));
}

HGDIOBJ SelectObject(HDC hdc, HGDIOBJ obj) {
    DcObject* dc = As<DcObject>(hdc, GdiType::Dc);
    if (!dc) return nullptr;
    HGDIOBJ previous = dc->selected;
    if (As<BitmapObject>(obj, GdiType::Bitmap)) dc->selected = obj;
    return previous;
}

BOOL DeleteObject(HGDIOBJ obj) {
    GdiObject* g = AsGdi(obj);
    if (!g) return FALSE;
    if (g->type == GdiType::Brush && static_cast<BrushObject*>(g)->stock) return TRUE;
    if (g->type == GdiType::Dc) return FALSE;
    delete g;
    return TRUE;
}

int GetObjectW(HANDLE obj, int size, LPVOID out) {
    BitmapObject* b = As<BitmapObject>(obj, GdiType::Bitmap);
    if (!b || !out || size < static_cast<int>(sizeof(BITMAP))) return 0;
    BITMAP* bm = static_cast<BITMAP*>(out);
    bm->bmType = 0;
    bm->bmWidth = b->width;
    bm->bmHeight = b->height;
    bm->bmBitsPixel = static_cast<WORD>(b->bitsPerPixel);
    bm->bmPlanes = 1;
    bm->bmWidthBytes = b->bitsPerPixel == 32 ? b->width * 4 : ((b->width + 15) / 16) * 2;
    bm->bmBits = nullptr;
    return sizeof(BITMAP);
}

int GetDIBits(HDC, HBITMAP bitmap, UINT start, UINT lines, LPVOID bits, LPBITMAPINFO bmi, UINT) {
    BitmapObject* b = As<BitmapObject>(bitmap, GdiType::Bitmap);
    if (!b || !bmi) return 0;
    if (!bits) {
        bmi->bmiHeader.biWidth = b->width;
        bmi->bmiHeader.biHeight = b->height;
        bmi->bmiHeader.biBitCount = static_cast<WORD>(b->bitsPerPixel);
        bmi->bmiHeader.biPlanes = 1;
        bmi->bmiHeader.biSizeImage = static_cast<DWORD>(b->width * b->height * 4);
        return b->height;
    }
    if (bmi->bmiHeader.biBitCount != 32) return 0;
    const bool topDown = bmi->bmiHeader.biHeight < 0;
    const int outWidth = bmi->bmiHeader.biWidth > 0 ? bmi->bmiHeader.biWidth : b->width;
    uint8_t* dst = static_cast<uint8_t*>(bits);
    UINT copied = 0;
    for (UINT i = 0; i < lines; ++i) {
        const int line = static_cast<int>(start + i);
        if (line >= b->height) break;
        // Scan line `line` counts from the top for top-down requests and from
        // the bottom otherwise.
        const int srcY = topDown ? line : (b->height - 1 - line);
        uint8_t* outRow = dst + static_cast<size_t>(i) * outWidth * 4;
        for (int x = 0; x < outWidth; ++x) {
            uint8_t* o = outRow + x * 4;
            if (x >= b->width) {
                o[0] = o[1] = o[2] = o[3] = 0;
                continue;
            }
            if (b->bitsPerPixel == 32) {
                const uint8_t* p = &b->pixels[(static_cast<size_t>(srcY) * b->width + x) * 4];
                o[0] = p[0];
                o[1] = p[1];
                o[2] = p[2];
                o[3] = p[3];
            } else {
                const uint8_t v = b->pixels[static_cast<size_t>(srcY) * b->width + x] ? 0xFF : 0x00;
                o[0] = o[1] = o[2] = v;
                o[3] = 0;
            }
        }
        ++copied;
    }
    return static_cast<int>(copied);
}

LONG GetBitmapBits(HBITMAP bitmap, LONG count, LPVOID bits) {
    BitmapObject* b = As<BitmapObject>(bitmap, GdiType::Bitmap);
    if (!b || !bits || count <= 0) return 0;
    if (b->bitsPerPixel == 32) {
        const LONG n = std::min<LONG>(count, static_cast<LONG>(b->pixels.size()));
        std::memcpy(bits, b->pixels.data(), static_cast<size_t>(n));
        return n;
    }
    // 1 bpp, rows padded to 16 bits.
    const size_t stride = ((static_cast<size_t>(b->width) + 15) / 16) * 2;
    std::vector<uint8_t> packed(stride * b->height, 0);
    for (int y = 0; y < b->height; ++y) {
        for (int x = 0; x < b->width; ++x) {
            if (b->pixels[static_cast<size_t>(y) * b->width + x]) packed[y * stride + x / 8] |= static_cast<uint8_t>(0x80 >> (x % 8));
        }
    }
    const LONG n = std::min<LONG>(count, static_cast<LONG>(packed.size()));
    std::memcpy(bits, packed.data(), static_cast<size_t>(n));
    return n;
}

// Copies a captured frame of another application's window into the bitmap
// selected into `dst` (ScreenCaptureKit; see os/mac_window_capture.h).
static BOOL CaptureForeignWindow(HWND window, HDC dst, int x, int y, int cx, int cy) {
    uint32_t windowId = 0;
    DcObject* dc = As<DcObject>(dst, GdiType::Dc);
    BitmapObject* b = dc ? As<BitmapObject>(dc->selected, GdiType::Bitmap) : nullptr;
    if (!MacWin32::ForeignWindowId(window, windowId) || !b || b->bitsPerPixel != 32) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    x = std::max(0, x);
    y = std::max(0, y);
    cx = std::min(cx, b->width - x);
    cy = std::min(cy, b->height - y);
    if (cx <= 0 || cy <= 0) return FALSE;
    uint8_t* origin = b->pixels.data() + (static_cast<size_t>(y) * b->width + x) * 4;
    if (!MacWindowCapture::CopyFrame(windowId, cx, cy, origin, static_cast<size_t>(b->width) * 4)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return TRUE;
}

BOOL BitBlt(HDC dst, int x, int y, int cx, int cy, HDC src, int, int, DWORD) {
    DcObject* source = As<DcObject>(src, GdiType::Dc);
    if (!source || !source->window) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    return CaptureForeignWindow(source->window, dst, x, y, cx, cy);
}

BOOL PrintWindow(HWND hwnd, HDC hdc, UINT) {
    DcObject* dc = As<DcObject>(hdc, GdiType::Dc);
    BitmapObject* b = dc ? As<BitmapObject>(dc->selected, GdiType::Bitmap) : nullptr;
    if (!b) return FALSE;
    return CaptureForeignWindow(hwnd, hdc, 0, 0, b->width, b->height);
}

int FillRect(HDC hdc, const RECT* rect, HBRUSH brush) {
    DcObject* dc = As<DcObject>(hdc, GdiType::Dc);
    BrushObject* br = As<BrushObject>(brush, GdiType::Brush);
    BitmapObject* b = dc ? As<BitmapObject>(dc->selected, GdiType::Bitmap) : nullptr;
    if (!b || !br || !rect || b->bitsPerPixel != 32) return 0;
    if (br->hollow) return 1;
    for (int y = std::max(0, static_cast<int>(rect->top)); y < std::min(b->height, static_cast<int>(rect->bottom)); ++y) {
        for (int x = std::max(0, static_cast<int>(rect->left)); x < std::min(b->width, static_cast<int>(rect->right)); ++x) {
            uint8_t* p = &b->pixels[(static_cast<size_t>(y) * b->width + x) * 4];
            p[0] = GetBValue(br->color);
            p[1] = GetGValue(br->color);
            p[2] = GetRValue(br->color);
            p[3] = 255;
        }
    }
    return 1;
}

HGDIOBJ GetStockObject(int index) {
    static BrushObject white, lightGray, gray, darkGray, black, nullBrush;
    static std::once_flag once;
    std::call_once(once, [] {
        // Windows stock brush colors.
        white.stock = lightGray.stock = gray.stock = darkGray.stock = black.stock = nullBrush.stock = true;
        white.color = RGB(255, 255, 255);
        lightGray.color = RGB(192, 192, 192);
        gray.color = RGB(128, 128, 128);
        darkGray.color = RGB(64, 64, 64);
        black.color = RGB(0, 0, 0);
        nullBrush.hollow = true;
    });
    switch (index) {
    case WHITE_BRUSH: return &white;
    case LTGRAY_BRUSH: return &lightGray;
    case GRAY_BRUSH: return &gray;
    case DKGRAY_BRUSH: return &darkGray;
    case BLACK_BRUSH: return &black;
    case NULL_BRUSH: return &nullBrush;
    default: return nullptr;
    }
}

int SetROP2(HDC hdc, int mode) {
    DcObject* dc = As<DcObject>(hdc, GdiType::Dc);
    if (!dc) return 0;
    const int previous = dc->rop2;
    dc->rop2 = mode;
    return previous;
}

int GetDeviceCaps(HDC hdc, int index) {
    double scale = MacOs::PrimaryDisplayScale();
    HWND window = WindowFromDC(hdc);
    if (!window) window = MacWin32::GameWindow();
    if (window) scale = MacWin32::GetWindowSnapshot(window).scale;
    switch (index) {
    case LOGPIXELSX:
    case LOGPIXELSY: return static_cast<int>(std::lround(USER_DEFAULT_SCREEN_DPI * scale));
    case BITSPIXEL: return 32;
    case HORZRES:
    case VERTRES: {
        const auto displays = MacOs::Displays();
        if (displays.empty()) return 0;
        return index == HORZRES ? displays[0].frame.Width() : displays[0].frame.Height();
    }
    default: return 0;
    }
}

HRGN CreateRectRgn(int x1, int y1, int x2, int y2) {
    auto* r = new RegionObject();
    r->rect = RECT{ x1, y1, x2, y2 };
    return reinterpret_cast<HRGN>(r);
}

} // extern "C"
