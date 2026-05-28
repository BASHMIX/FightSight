#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace fightsight {

class ConfigManager {
public:
    // Load config from disk. If the file is missing or invalid, populate
    // m_json with defaults and return false.
    bool Load(const std::string& path);
    bool Save(const std::string& path) const;

    const nlohmann::json& Json() const { return m_json; }
    nlohmann::json&       Json()       { return m_json; }

    std::string SpoutReceiverName() const;

private:
    void ApplyDefaults();
    nlohmann::json m_json;
};

} // namespace fightsight
