#pragma once

#include <cctype>
#include <string>

namespace fightsight {

// Convert an ROI name into a token safe to use in filenames AND in
// Streamer.bot action names (alphanumerics + underscores only).
inline std::string SanitizeName(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == '_') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    if (out.empty()) out = "Unnamed";
    return out;
}

} // namespace fightsight
