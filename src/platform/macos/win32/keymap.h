#pragma once

#include <cstdint>
#include <string>

// Conversions between macOS virtual key codes (kVK_*, which GLFW passes as
// "scancode" on macOS), GLFW key codes, Windows Set-1 scan codes and Windows
// virtual-key codes.
//
// Scan codes use bit 8 for the E0 (extended) prefix, e.g. 0x11D = right Ctrl,
// matching how Win32 encodes bits 16-24 of a keystroke lParam.
namespace MacKeymap {

constexpr unsigned kExtended = 0x100;

int GlfwKeyFromMacKeyCode(int macKeyCode);
int MacKeyCodeFromGlfwKey(int glfwKey);

unsigned ScanCodeFromMacKeyCode(int macKeyCode);
// -1 when the scan code has no key on Mac keyboards.
int MacKeyCodeFromScanCode(unsigned scanCode);
// GLFW key for a scan code, including keys Mac keyboards do not have.
int GlfwKeyFromScanCode(unsigned scanCode);

// Windows VK for a Mac key. Character keys follow the current layout like
// Windows does; modifiers return their sided VK (VK_LSHIFT, VK_RMENU, ...).
unsigned VkFromMacKeyCode(int macKeyCode);
// -1 when no Mac key produces this VK. Generic modifiers map to the left key.
int MacKeyCodeFromVk(unsigned vk);
unsigned ScanCodeFromVk(unsigned vk);
// MAPVK_VSC_TO_VK (sided == false) or MAPVK_VSC_TO_VK_EX (sided == true).
unsigned VkFromScanCode(unsigned scanCode, bool sided);

// WM_KEYDOWN carries VK_SHIFT/VK_CONTROL/VK_MENU for modifiers.
unsigned GenericVk(unsigned vk);

// Win32 GetKeyNameText semantics for a scan code.
std::string KeyName(unsigned scanCode);

// Low word: Windows LANGID describing the layout (English means ANSI
// keyboard); high bits change whenever the input source changes.
uintptr_t KeyboardLayoutHandle();

} // namespace MacKeymap
