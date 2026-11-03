#pragma once
#include <string>
#include <string_view>

namespace st {

// Persisted in settings.json in config_dir(). SUPERTERMINAL_MODEL overrides the
// model for one session without being saved.
struct Settings {
    std::string model = "claude-haiku-5-5";  // only the three 5.5 models are allowed
    std::string effort = "low";              // low | medium | high | xhigh | max
    bool autorun = true;                     // run read-only translations without asking
    std::string shell;                       // default for --shell; empty = platform default
};

Settings load_settings();
bool save_settings(const Settings& s);

// "haiku" / "sonnet" / "opus" (or their 5.5 ids) -> model id; empty for anything else.
std::string resolve_model(std::string_view name);
bool valid_effort(std::string_view effort);

}  // namespace st
