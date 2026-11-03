#include "settings.h"

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "json_util.h"
#include "util.h"

using nlohmann::json;

namespace st {

static std::filesystem::path settings_path() { return config_dir() / "settings.json"; }

std::string resolve_model(std::string_view name) {
    std::string n = to_lower(trim(name));
    if (n == "opus" || n == "opus-5.5" || n == "opus 5.5" || n == "claude-opus-5-5") return "claude-opus-5-5";
    if (n == "sonnet" || n == "sonnet-5.5" || n == "sonnet 5.5" || n == "claude-sonnet-5-5") return "claude-sonnet-5-5";
    if (n == "haiku" || n == "haiku-5.5" || n == "haiku 5.5" || n == "claude-haiku-5-5") return "claude-haiku-5-5";
    return {};
}

bool valid_effort(std::string_view e) {
    return e == "low" || e == "medium" || e == "high" || e == "xhigh" || e == "max";
}

Settings load_settings() {
    Settings s;
    std::ifstream f(settings_path());
    if (f) {
        std::stringstream ss;
        ss << f.rdbuf();
        json j = json::parse(ss.str(), nullptr, false);
        if (j.is_object()) {
            if (auto m = resolve_model(jstr(j, "model")); !m.empty()) s.model = m;
            if (auto e = jstr(j, "effort"); valid_effort(e)) s.effort = e;
            s.autorun = jbool(j, "autorun", s.autorun);
            s.shell = jstr(j, "shell");
        }
    }
    if (auto m = resolve_model(read_env("SUPERTERMINAL_MODEL")); !m.empty()) s.model = m;
    return s;
}

bool save_settings(const Settings& s) {
    json j = {{"model", s.model}, {"effort", s.effort}, {"autorun", s.autorun}};
    if (!s.shell.empty()) j["shell"] = s.shell;
    std::ofstream f(settings_path(), std::ios::trunc);
    if (!f) return false;
    f << j.dump(2) << "\n";
    return static_cast<bool>(f);
}

}  // namespace st
