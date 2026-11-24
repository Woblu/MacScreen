#include "platform/macos/win32/keymap.h"

#include "platform/macos/os/mac_os.h"
#include "platform/macos/win32/win32_internal.h"
#include "platform/macos/win32/win32_types.h"

#include <array>
#include <cctype>
#include <mutex>

namespace MacKeymap {
namespace {

constexpr int kNone = -1;

struct KeyRow {
    int macKeyCode;   // kVK_* or kNone for keys Mac keyboards lack
    int glfwKey;      // GLFW_KEY_* (matches GLFW's Cocoa key table)
    unsigned scan;    // Set-1 scan code, kExtended for E0
    unsigned usVk;    // Windows VK on a US layout
};

constexpr unsigned E(unsigned code) { return code | kExtended; }

// GLFW key constants
enum : int {
    K_SPACE = 32, K_APOSTROPHE = 39, K_COMMA = 44, K_MINUS = 45, K_PERIOD = 46, K_SLASH = 47,
    K_0 = 48, K_SEMICOLON = 59, K_EQUAL = 61, K_A = 65, K_LEFT_BRACKET = 91, K_BACKSLASH = 92,
    K_RIGHT_BRACKET = 93, K_GRAVE = 96, K_WORLD_1 = 161, K_WORLD_2 = 162, K_ESCAPE = 256, K_ENTER = 257,
    K_TAB = 258, K_BACKSPACE = 259, K_INSERT = 260, K_DELETE = 261, K_RIGHT = 262, K_LEFT = 263, K_DOWN = 264,
    K_UP = 265, K_PAGE_UP = 266, K_PAGE_DOWN = 267, K_HOME = 268, K_END = 269, K_CAPS_LOCK = 280,
    K_SCROLL_LOCK = 281, K_NUM_LOCK = 282, K_PRINT_SCREEN = 283, K_PAUSE = 284, K_F1 = 290,
    K_KP_0 = 320, K_KP_DECIMAL = 330, K_KP_DIVIDE = 331, K_KP_MULTIPLY = 332, K_KP_SUBTRACT = 333,
    K_KP_ADD = 334, K_KP_ENTER = 335, K_KP_EQUAL = 336, K_LEFT_SHIFT = 340, K_LEFT_CONTROL = 341,
    K_LEFT_ALT = 342, K_LEFT_SUPER = 343, K_RIGHT_SHIFT = 344, K_RIGHT_CONTROL = 345, K_RIGHT_ALT = 346,
    K_RIGHT_SUPER = 347, K_MENU = 348, K_UNKNOWN = -1,
};


const KeyRow kRows[] = {
    { 0x00, K_A + 0, 0x1E, 'A' },  { 0x01, K_A + 18, 0x1F, 'S' }, { 0x02, K_A + 3, 0x20, 'D' },
    { 0x03, K_A + 5, 0x21, 'F' },  { 0x04, K_A + 7, 0x23, 'H' },  { 0x05, K_A + 6, 0x22, 'G' },
    { 0x06, K_A + 25, 0x2C, 'Z' }, { 0x07, K_A + 23, 0x2D, 'X' }, { 0x08, K_A + 2, 0x2E, 'C' },
    { 0x09, K_A + 21, 0x2F, 'V' }, { 0x0A, K_WORLD_1, 0x56, VK_OEM_102 }, { 0x0B, K_A + 1, 0x30, 'B' },
    { 0x0C, K_A + 16, 0x10, 'Q' }, { 0x0D, K_A + 22, 0x11, 'W' }, { 0x0E, K_A + 4, 0x12, 'E' },
    { 0x0F, K_A + 17, 0x13, 'R' }, { 0x10, K_A + 24, 0x15, 'Y' }, { 0x11, K_A + 19, 0x14, 'T' },
    { 0x12, K_0 + 1, 0x02, '1' },  { 0x13, K_0 + 2, 0x03, '2' },  { 0x14, K_0 + 3, 0x04, '3' },
    { 0x15, K_0 + 4, 0x05, '4' },  { 0x16, K_0 + 6, 0x07, '6' },  { 0x17, K_0 + 5, 0x06, '5' },
    { 0x18, K_EQUAL, 0x0D, VK_OEM_PLUS }, { 0x19, K_0 + 9, 0x0A, '9' }, { 0x1A, K_0 + 7, 0x08, '7' },
    { 0x1B, K_MINUS, 0x0C, VK_OEM_MINUS }, { 0x1C, K_0 + 8, 0x09, '8' }, { 0x1D, K_0 + 0, 0x0B, '0' },
    { 0x1E, K_RIGHT_BRACKET, 0x1B, VK_OEM_6 }, { 0x1F, K_A + 14, 0x18, 'O' }, { 0x20, K_A + 20, 0x16, 'U' },
    { 0x21, K_LEFT_BRACKET, 0x1A, VK_OEM_4 }, { 0x22, K_A + 8, 0x17, 'I' }, { 0x23, K_A + 15, 0x19, 'P' },
    { 0x24, K_ENTER, 0x1C, VK_RETURN }, { 0x25, K_A + 11, 0x26, 'L' }, { 0x26, K_A + 9, 0x24, 'J' },
    { 0x27, K_APOSTROPHE, 0x28, VK_OEM_7 }, { 0x28, K_A + 10, 0x25, 'K' }, { 0x29, K_SEMICOLON, 0x27, VK_OEM_1 },
    { 0x2A, K_BACKSLASH, 0x2B, VK_OEM_5 }, { 0x2B, K_COMMA, 0x33, VK_OEM_COMMA }, { 0x2C, K_SLASH, 0x35, VK_OEM_2 },
    { 0x2D, K_A + 13, 0x31, 'N' }, { 0x2E, K_A + 12, 0x32, 'M' }, { 0x2F, K_PERIOD, 0x34, VK_OEM_PERIOD },
    { 0x30, K_TAB, 0x0F, VK_TAB }, { 0x31, K_SPACE, 0x39, VK_SPACE }, { 0x32, K_GRAVE, 0x29, VK_OEM_3 },
    { 0x33, K_BACKSPACE, 0x0E, VK_BACK }, { 0x35, K_ESCAPE, 0x01, VK_ESCAPE },
    { 0x36, K_RIGHT_SUPER, E(0x5C), VK_RWIN }, { 0x37, K_LEFT_SUPER, E(0x5B), VK_LWIN },
    { 0x38, K_LEFT_SHIFT, 0x2A, VK_LSHIFT }, { 0x39, K_CAPS_LOCK, 0x3A, VK_CAPITAL },
    { 0x3A, K_LEFT_ALT, 0x38, VK_LMENU }, { 0x3B, K_LEFT_CONTROL, 0x1D, VK_LCONTROL },
    { 0x3C, K_RIGHT_SHIFT, 0x36, VK_RSHIFT }, { 0x3D, K_RIGHT_ALT, E(0x38), VK_RMENU },
    { 0x3E, K_RIGHT_CONTROL, E(0x1D), VK_RCONTROL },
    { 0x40, K_F1 + 16, 0x68, VK_F17 }, { 0x41, K_KP_DECIMAL, 0x53, VK_DECIMAL },
    { 0x43, K_KP_MULTIPLY, 0x37, VK_MULTIPLY }, { 0x45, K_KP_ADD, 0x4E, VK_ADD },
    { 0x47, K_NUM_LOCK, E(0x45), VK_NUMLOCK }, { 0x48, K_UNKNOWN, E(0x30), VK_VOLUME_UP },
    { 0x49, K_UNKNOWN, E(0x2E), VK_VOLUME_DOWN }, { 0x4A, K_UNKNOWN, E(0x20), VK_VOLUME_MUTE },
    { 0x4B, K_KP_DIVIDE, E(0x35), VK_DIVIDE }, { 0x4C, K_KP_ENTER, E(0x1C), VK_RETURN },
    { 0x4E, K_KP_SUBTRACT, 0x4A, VK_SUBTRACT }, { 0x4F, K_F1 + 17, 0x69, VK_F18 }, { 0x50, K_F1 + 18, 0x6A, VK_F19 },
    { 0x51, K_KP_EQUAL, 0x59, VK_OEM_NEC_EQUAL }, { 0x52, K_KP_0 + 0, 0x52, VK_NUMPAD0 },
    { 0x53, K_KP_0 + 1, 0x4F, VK_NUMPAD1 }, { 0x54, K_KP_0 + 2, 0x50, VK_NUMPAD2 },
    { 0x55, K_KP_0 + 3, 0x51, VK_NUMPAD3 }, { 0x56, K_KP_0 + 4, 0x4B, VK_NUMPAD4 },
    { 0x57, K_KP_0 + 5, 0x4C, VK_NUMPAD5 }, { 0x58, K_KP_0 + 6, 0x4D, VK_NUMPAD6 },
    { 0x59, K_KP_0 + 7, 0x47, VK_NUMPAD7 }, { 0x5A, K_F1 + 19, 0x6B, VK_F20 },
    { 0x5B, K_KP_0 + 8, 0x48, VK_NUMPAD8 }, { 0x5C, K_KP_0 + 9, 0x49, VK_NUMPAD9 },
    { 0x5D, K_UNKNOWN, 0x7D, VK_OEM_5 }, { 0x5E, K_UNKNOWN, 0x73, VK_OEM_102 },
    { 0x5F, K_UNKNOWN, 0x7E, VK_SEPARATOR }, { 0x60, K_F1 + 4, 0x3F, VK_F5 }, { 0x61, K_F1 + 5, 0x40, VK_F6 },
    { 0x62, K_F1 + 6, 0x41, VK_F7 }, { 0x63, K_F1 + 2, 0x3D, VK_F3 }, { 0x64, K_F1 + 7, 0x42, VK_F8 },
    { 0x65, K_F1 + 8, 0x43, VK_F9 }, { 0x66, K_UNKNOWN, 0x7B, VK_NONCONVERT }, { 0x67, K_F1 + 10, 0x57, VK_F11 },
    { 0x68, K_UNKNOWN, 0x70, VK_KANA }, { 0x69, K_F1 + 12, 0x64, VK_F13 }, { 0x6A, K_F1 + 15, 0x67, VK_F16 },
    { 0x6B, K_F1 + 13, 0x65, VK_F14 }, { 0x6D, K_F1 + 9, 0x44, VK_F10 }, { 0x6E, K_MENU, E(0x5D), VK_APPS },
    { 0x6F, K_F1 + 11, 0x58, VK_F12 }, { 0x71, K_F1 + 14, 0x66, VK_F15 }, { 0x72, K_INSERT, E(0x52), VK_INSERT },
    { 0x73, K_HOME, E(0x47), VK_HOME }, { 0x74, K_PAGE_UP, E(0x49), VK_PRIOR }, { 0x75, K_DELETE, E(0x53), VK_DELETE },
    { 0x76, K_F1 + 3, 0x3E, VK_F4 }, { 0x77, K_END, E(0x4F), VK_END }, { 0x78, K_F1 + 1, 0x3C, VK_F2 },
    { 0x79, K_PAGE_DOWN, E(0x51), VK_NEXT }, { 0x7A, K_F1 + 0, 0x3B, VK_F1 }, { 0x7B, K_LEFT, E(0x4B), VK_LEFT },
    { 0x7C, K_RIGHT, E(0x4D), VK_RIGHT }, { 0x7D, K_DOWN, E(0x50), VK_DOWN }, { 0x7E, K_UP, E(0x48), VK_UP },
    // Keys Mac keyboards lack. Rebinds can still output them to the game.
    { kNone, K_PRINT_SCREEN, E(0x37), VK_SNAPSHOT }, { kNone, K_SCROLL_LOCK, 0x46, VK_SCROLL },
    { kNone, K_PAUSE, 0x45, VK_PAUSE }, { kNone, K_F1 + 20, 0x6C, VK_F21 }, { kNone, K_F1 + 21, 0x6D, VK_F22 },
    { kNone, K_F1 + 22, 0x6E, VK_F23 }, { kNone, K_F1 + 23, 0x76, VK_F24 },
};

const KeyRow* RowForMac(int macKeyCode) {
    for (const KeyRow& r : kRows) {
        if (r.macKeyCode == macKeyCode && macKeyCode != kNone) return &r;
    }
    return nullptr;
}

const KeyRow* RowForScan(unsigned scan) {
    for (const KeyRow& r : kRows) {
        if (r.scan == scan) return &r;
    }
    return nullptr;
}

unsigned VkForAsciiPunctuation(char16_t c) {
    switch (c) {
    case ';': return VK_OEM_1;
    case '=': case '+': return VK_OEM_PLUS;
    case ',': return VK_OEM_COMMA;
    case '-': return VK_OEM_MINUS;
    case '.': return VK_OEM_PERIOD;
    case '/': return VK_OEM_2;
    case '`': return VK_OEM_3;
    case '[': return VK_OEM_4;
    case '\\': return VK_OEM_5;
    case ']': return VK_OEM_6;
    case '\'': return VK_OEM_7;
    default: return 0;
    }
}

bool IsCharacterKey(const KeyRow& r) {
    const unsigned vk = r.usVk;
    return (vk >= 'A' && vk <= 'Z') || (vk >= VK_OEM_1 && vk <= VK_OEM_3) || (vk >= VK_OEM_4 && vk <= VK_OEM_7) ||
           vk == VK_OEM_102;
}

struct LayoutTable {
    uint64_t layoutId = 0;
    std::array<unsigned, 128> vkForMac{};
};

std::mutex g_tableMutex;
LayoutTable g_table;

// Returned by value: another thread may rebuild the table after a layout change.
LayoutTable CurrentTable() {
    const uint64_t layoutId = MacOs::KeyboardLayoutId();
    std::lock_guard<std::mutex> lock(g_tableMutex);
    if (g_table.layoutId == layoutId && layoutId != 0) return g_table;

    LayoutTable t;
    t.layoutId = layoutId;
    for (const KeyRow& r : kRows) {
        if (r.macKeyCode < 0 || r.macKeyCode >= 128) continue;
        unsigned vk = r.usVk;
        if (IsCharacterKey(r)) {
            const std::u16string chars = MacOs::TranslateKey(static_cast<uint16_t>(r.macKeyCode), false, false, false, false);
            if (chars.size() == 1) {
                const char16_t c = chars[0];
                if (c >= 'a' && c <= 'z') vk = static_cast<unsigned>(c - 'a' + 'A');
                else if (c >= 'A' && c <= 'Z') vk = static_cast<unsigned>(c);
                else if (unsigned p = VkForAsciiPunctuation(c)) vk = p;
            }
        }
        t.vkForMac[static_cast<size_t>(r.macKeyCode)] = vk;
    }
    g_table = t;
    return g_table;
}

uint16_t LangIdForLanguage(const std::string& language) {
    const std::string lang = language.substr(0, language.find_first_of("-_"));
    struct Entry {
        const char* code;
        uint16_t langId;
    };
    static const Entry kEntries[] = {
        { "en", 0x0409 }, { "de", 0x0407 }, { "fr", 0x040C }, { "es", 0x0C0A }, { "it", 0x0410 }, { "nl", 0x0413 },
        { "pt", 0x0816 }, { "sv", 0x041D }, { "nb", 0x0414 }, { "no", 0x0414 }, { "da", 0x0406 }, { "fi", 0x040B },
        { "pl", 0x0415 }, { "cs", 0x0405 }, { "sk", 0x041B }, { "hu", 0x040E }, { "tr", 0x041F }, { "ru", 0x0419 },
        { "uk", 0x0422 }, { "ja", 0x0411 }, { "ko", 0x0412 }, { "zh", 0x0804 }, { "el", 0x0408 }, { "he", 0x040D },
        { "ar", 0x0401 }, { "is", 0x040F }, { "et", 0x0425 }, { "lv", 0x0426 }, { "lt", 0x0427 }, { "ro", 0x0418 },
        { "sl", 0x0424 }, { "hr", 0x041A }, { "bg", 0x0402 }, { "sr", 0x081A },
    };
    for (const Entry& e : kEntries) {
        if (lang == e.code) return e.langId;
    }
    return 0x0409;
}

} // namespace

int GlfwKeyFromMacKeyCode(int macKeyCode) {
    const KeyRow* r = RowForMac(macKeyCode);
    return r ? r->glfwKey : K_UNKNOWN;
}

int MacKeyCodeFromGlfwKey(int glfwKey) {
    for (const KeyRow& r : kRows) {
        if (r.glfwKey == glfwKey && r.macKeyCode != kNone && glfwKey != K_UNKNOWN) return r.macKeyCode;
    }
    return kNone;
}

unsigned ScanCodeFromMacKeyCode(int macKeyCode) {
    const KeyRow* r = RowForMac(macKeyCode);
    return r ? r->scan : 0;
}

int MacKeyCodeFromScanCode(unsigned scanCode) {
    const KeyRow* r = RowForScan(scanCode);
    if (!r && (scanCode & kExtended) == 0) {
        // Some callers omit the E0 flag for extended keys.
        r = RowForScan(scanCode | kExtended);
    }
    return r ? r->macKeyCode : kNone;
}

int GlfwKeyFromScanCode(unsigned scanCode) {
    const KeyRow* r = RowForScan(scanCode);
    if (!r && (scanCode & kExtended) == 0) r = RowForScan(scanCode | kExtended);
    return r ? r->glfwKey : K_UNKNOWN;
}

unsigned VkFromMacKeyCode(int macKeyCode) {
    if (macKeyCode < 0 || macKeyCode >= 128) return 0;
    return CurrentTable().vkForMac[static_cast<size_t>(macKeyCode)];
}

int MacKeyCodeFromVk(unsigned vk) {
    switch (vk) {
    case VK_SHIFT: return 0x38;
    case VK_CONTROL: return 0x3B;
    case VK_MENU: return 0x3A;
    default: break;
    }
    const LayoutTable& t = CurrentTable();
    for (int code = 0; code < 128; ++code) {
        if (t.vkForMac[static_cast<size_t>(code)] == vk && vk != 0) return code;
    }
    return kNone;
}

unsigned ScanCodeFromVk(unsigned vk) {
    const int mac = MacKeyCodeFromVk(vk);
    if (mac != kNone) return ScanCodeFromMacKeyCode(mac);
    for (const KeyRow& r : kRows) {
        if (r.macKeyCode == kNone && r.usVk == vk) return r.scan;
    }
    return 0;
}

unsigned VkFromScanCode(unsigned scanCode, bool sided) {
    const KeyRow* r = RowForScan(scanCode);
    if (!r && (scanCode & kExtended) == 0) r = RowForScan(scanCode | kExtended);
    if (!r) return 0;
    const unsigned vk = r->macKeyCode != kNone ? VkFromMacKeyCode(r->macKeyCode) : r->usVk;
    return sided ? vk : GenericVk(vk);
}

unsigned GenericVk(unsigned vk) {
    switch (vk) {
    case VK_LSHIFT: case VK_RSHIFT: return VK_SHIFT;
    case VK_LCONTROL: case VK_RCONTROL: return VK_CONTROL;
    case VK_LMENU: case VK_RMENU: return VK_MENU;
    default: return vk;
    }
}


std::string KeyName(unsigned scanCode) {
    struct Name {
        unsigned scan;
        const char* name;
    };
    static const Name kNames[] = {
        { 0x01, "Esc" }, { 0x0E, "Backspace" }, { 0x0F, "Tab" }, { 0x1C, "Enter" }, { 0x1D, "Ctrl" }, { 0x2A, "Shift" },
        { 0x36, "Right Shift" }, { 0x37, "Num *" }, { 0x38, "Alt" }, { 0x39, "Space" }, { 0x3A, "Caps Lock" },
        { 0x3B, "F1" }, { 0x3C, "F2" }, { 0x3D, "F3" }, { 0x3E, "F4" }, { 0x3F, "F5" }, { 0x40, "F6" }, { 0x41, "F7" },
        { 0x42, "F8" }, { 0x43, "F9" }, { 0x44, "F10" }, { 0x45, "Pause" }, { 0x46, "Scroll Lock" }, { 0x47, "Num 7" },
        { 0x48, "Num 8" }, { 0x49, "Num 9" }, { 0x4A, "Num -" }, { 0x4B, "Num 4" }, { 0x4C, "Num 5" }, { 0x4D, "Num 6" },
        { 0x4E, "Num +" }, { 0x4F, "Num 1" }, { 0x50, "Num 2" }, { 0x51, "Num 3" }, { 0x52, "Num 0" }, { 0x53, "Num Del" },
        { 0x57, "F11" }, { 0x58, "F12" }, { 0x59, "Num =" }, { 0x64, "F13" }, { 0x65, "F14" }, { 0x66, "F15" },
        { 0x67, "F16" }, { 0x68, "F17" }, { 0x69, "F18" }, { 0x6A, "F19" }, { 0x6B, "F20" }, { 0x6C, "F21" },
        { 0x6D, "F22" }, { 0x6E, "F23" }, { 0x76, "F24" }, { E(0x1C), "Num Enter" }, { E(0x1D), "Right Ctrl" },
        { E(0x35), "Num /" }, { E(0x37), "Prnt Scrn" }, { E(0x38), "Right Alt" }, { E(0x45), "Num Lock" },
        { E(0x47), "Home" }, { E(0x48), "Up" }, { E(0x49), "Page Up" }, { E(0x4B), "Left" }, { E(0x4D), "Right" },
        { E(0x4F), "End" }, { E(0x50), "Down" }, { E(0x51), "Page Down" }, { E(0x52), "Insert" }, { E(0x53), "Delete" },
        { E(0x5B), "Left Windows" }, { E(0x5C), "Right Windows" }, { E(0x5D), "Application" },
    };
    for (const Name& n : kNames) {
        if (n.scan == scanCode) return n.name;
    }
    const int mac = MacKeyCodeFromScanCode(scanCode);
    if (mac == kNone) return {};
    std::u16string chars = MacOs::TranslateKey(static_cast<uint16_t>(mac), false, false, false, false);
    if (chars.empty()) return {};
    std::string name = MacWin32::WideToUtf8(std::wstring(chars.begin(), chars.end()));
    if (name.size() == 1) name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    return name;
}

uintptr_t KeyboardLayoutHandle() {
    constexpr uint16_t kLangEnglish = 0x09;
    constexpr uint16_t kLangNeutralNonEnglish = 0x3F;
    uint16_t langId = LangIdForLanguage(MacOs::KeyboardLayoutLanguage());
    // Toolscreen's keyboard view treats an English layout as an ANSI keyboard.
    // Macs report the physical keyboard type, so encode that instead.
    const bool ansiKeyboard = MacOs::PhysicalKeyboardType() == 0;
    const uint16_t primary = ansiKeyboard ? kLangEnglish
                                          : ((langId & 0x3FF) == kLangEnglish ? kLangNeutralNonEnglish : (langId & 0x3FF));
    langId = static_cast<uint16_t>((langId & 0xFC00) | primary);
    const uint64_t layoutId = MacOs::KeyboardLayoutId();
    return (static_cast<uintptr_t>(layoutId & 0xFFFFFFull) << 16) | langId;
}

} // namespace MacKeymap
