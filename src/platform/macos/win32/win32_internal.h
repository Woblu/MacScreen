#pragma once

// Helpers shared between the Win32 compatibility files and the macOS bridge.

#include "platform/macos/win32/win32.h"

#include <string>

namespace MacWin32 {

std::string WideToUtf8(const std::wstring& wide);
std::wstring Utf8ToWide(const std::string& utf8);
// Decodes UTF-8 into wchar_t (UTF-32). Returns false on invalid input when strict.
bool Utf8ToWide(const char* data, size_t length, std::wstring& out, bool strict);
void SetLastErrorFromErrno(int err);

// Toolscreen's own image (mach header), used as its HMODULE.
HMODULE ToolscreenModule();

} // namespace MacWin32
