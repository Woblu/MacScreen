#pragma once

#include <string>

namespace MacFonts {

// Maps a Windows system font path (e.g. "c:\Windows\Fonts\Arial.ttf", the
// default in Toolscreen configs) to the same font file in the macOS font
// folders. Returns an empty string when `path` is not a Windows font path or
// macOS has no font with that file name.
std::string MapWindowsFontPath(const std::string& path);

// The folder holding the .ttf system fonts (Arial, Tahoma, Verdana, ...); the
// counterpart of C:\Windows\Fonts for the system font list.
std::wstring SystemTtfFontDirectory();

} // namespace MacFonts
