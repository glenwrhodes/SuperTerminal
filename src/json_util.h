#pragma once
#include <string>

#include <nlohmann/json.hpp>

namespace st {

// Null-safe field access: PowerShell's ConvertTo-Json writes $null as null,
// and nlohmann's value() throws on a null that should be a string.
inline std::string jstr(const nlohmann::json& j, const char* key, const std::string& fallback = {}) {
    if (!j.is_object()) return fallback;
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : fallback;
}

inline int jint(const nlohmann::json& j, const char* key, int fallback = 0) {
    if (!j.is_object()) return fallback;
    auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<int>() : fallback;
}

inline bool jbool(const nlohmann::json& j, const char* key, bool fallback = false) {
    if (!j.is_object()) return fallback;
    auto it = j.find(key);
    return it != j.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

}  // namespace st
