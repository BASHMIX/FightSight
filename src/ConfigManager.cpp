#include "App/ConfigManager.h"

#include <fstream>

namespace fightsight {

void ConfigManager::ApplyDefaults() {
    m_json = nlohmann::json::object();
    m_json["version"]                  = 1;
    m_json["spout"]["receiver_name"]   = "";
    m_json["viewport"]["force_1080p"]  = false;
    m_json["websocket"]["host"]        = "127.0.0.1";
    m_json["websocket"]["port"]        = 8080;
    m_json["websocket"]["endpoint"]    = "/";
    m_json["rois"]                     = nlohmann::json::array();
}

bool ConfigManager::Load(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        ApplyDefaults();
        return false;
    }
    try {
        f >> m_json;
    } catch (const std::exception&) {
        ApplyDefaults();
        return false;
    }
    return true;
}

bool ConfigManager::Save(const std::string& path) const {
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << m_json.dump(2);
    return f.good();
}

std::string ConfigManager::SpoutReceiverName() const {
    if (m_json.contains("spout") && m_json["spout"].contains("receiver_name")) {
        return m_json["spout"]["receiver_name"].get<std::string>();
    }
    return {};
}

} // namespace fightsight
