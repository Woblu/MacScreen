#include "platform/macos/os/mac_fonts.h"

#include "platform/macos/os/mac_os.h"

#include <algorithm>
#include <cctype>
#include <sys/stat.h>

namespace MacFonts {

std::vector<std::string> SystemFontDirectories() { return MacOs::FontDirectories(); }

std::string MapWindowsFontPath(const std::string& path) {
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::replace(lower.begin(), lower.end(), '/', '\\');
    const std::string marker = "\\windows\\fonts\\";
    const size_t at = lower.find(marker);
    if (at == std::string::npos) return {};

    std::string name = path.substr(at + marker.size());
    std::replace(name.begin(), name.end(), '\\', '/');
    if (name.empty() || name.find('/') != std::string::npos) return {};

    for (const std::string& dir : SystemFontDirectories()) {
        const std::string candidate = dir + "/" + name;
        struct stat st {};
        if (stat(candidate.c_str(), &st) == 0 && S_ISREG(st.st_mode)) return candidate;
    }
    return {};
}

} // namespace MacFonts
