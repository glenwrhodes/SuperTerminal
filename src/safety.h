#pragma once
#include <string>

#include <nlohmann/json.hpp>

namespace st {

// May a command Claude wrote run without the user pressing Enter?
//
// Claude labels each command read_only / modifies / destructive, but that
// label alone never decides it. The command must also pass a local check on
// PowerShell's own parse of it: every command must resolve to a cmdlet with a
// read-only verb (Get, Select, Sort, Measure, ...) or to a small allow-list of
// read-only programs, with no file redirection and no method calls outside a
// read-only list. Anything else waits for Enter.
struct AutorunVerdict {
    bool allowed = false;
    std::string reason;
};

AutorunVerdict check_autorun(const nlohmann::json& analysis, const std::string& claude_risk);

}  // namespace st
