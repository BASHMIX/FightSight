#include "App/ProfileManager.h"
#include "App/Util.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace fightsight {

namespace fs = std::filesystem;

bool ProfileManager::Init(const std::string& profilesDir,
                          const std::string& templatesRoot) {
    m_profilesDir   = profilesDir;
    m_templatesRoot = templatesRoot;
    try {
        fs::create_directories(m_profilesDir);
        fs::create_directories(m_templatesRoot);
    } catch (...) {
        return false;
    }
    return true;
}

std::string ProfileManager::ProfilePath(const std::string& name) const {
    return m_profilesDir + "/" + name + ".json";
}

std::string ProfileManager::TemplatesDirFor(const std::string& name) const {
    return m_templatesRoot + "/" + name;
}

std::string ProfileManager::SanitizeProfileName(const std::string& in) {
    return SanitizeName(in);
}

std::vector<std::string> ProfileManager::ListProfiles() const {
    std::vector<std::string> out;
    try {
        if (!fs::exists(m_profilesDir)) return out;
        for (const auto& entry : fs::directory_iterator(m_profilesDir)) {
            if (!entry.is_regular_file()) continue;
            const auto p = entry.path();
            if (p.extension() != ".json") continue;
            out.push_back(p.stem().string());
        }
    } catch (...) {}
    std::sort(out.begin(), out.end(),
        [](const std::string& a, const std::string& b) {
            return std::lexicographical_compare(
                a.begin(), a.end(), b.begin(), b.end(),
                [](char x, char y) {
                    return std::tolower((unsigned char)x)
                         < std::tolower((unsigned char)y);
                });
        });
    return out;
}

bool ProfileManager::HasProfile(const std::string& name) const {
    if (name.empty()) return false;
    std::error_code ec;
    return fs::exists(ProfilePath(name), ec);
}

bool ProfileManager::LoadProfile(const std::string& name, RoiManager& rm) {
    if (name.empty()) return false;
    std::ifstream f(ProfilePath(name));
    if (!f.is_open()) return false;

    nlohmann::json j;
    try { f >> j; } catch (...) { return false; }

    // Accept either {"rois":[...]} envelope or a bare top-level array.
    if (j.is_array()) {
        rm.LoadFromJson(j);
    } else if (j.is_object() && j.contains("rois")) {
        rm.LoadFromJson(j["rois"]);
    } else {
        rm.LoadFromJson(nlohmann::json::array());
    }

    m_active = name;
    try { fs::create_directories(TemplatesDirFor(name)); } catch (...) {}
    return true;
}

bool ProfileManager::SaveProfile(const std::string& name,
                                 const RoiManager& rm) const {
    if (name.empty()) return false;

    nlohmann::json j;
    j["version"] = 1;
    j["name"]    = name;
    j["rois"]    = rm.ToJson();

    try { fs::create_directories(m_profilesDir); } catch (...) {}
    std::ofstream f(ProfilePath(name));
    if (!f.is_open()) return false;
    f << j.dump(2);
    return f.good();
}

bool ProfileManager::CreateProfile(const std::string& name) {
    if (name.empty()) return false;
    if (HasProfile(name)) return false;

    nlohmann::json j;
    j["version"] = 1;
    j["name"]    = name;
    j["rois"]    = nlohmann::json::array();

    try {
        fs::create_directories(m_profilesDir);
        fs::create_directories(TemplatesDirFor(name));
    } catch (...) {}

    std::ofstream f(ProfilePath(name));
    if (!f.is_open()) return false;
    f << j.dump(2);
    return f.good();
}

bool ProfileManager::DeleteProfile(const std::string& name) {
    if (name.empty()) return false;
    if (name == m_active) return false; // refuse - app still needs it
    std::error_code ec;
    return fs::remove(ProfilePath(name), ec) && !ec;
}

} // namespace fightsight
