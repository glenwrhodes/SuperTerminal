#pragma once
#include <string>

#include <nlohmann/json.hpp>

namespace st {

// Decides, for each line typed at the prompt, whether PowerShell should run it
// as-is or Claude should interpret it.
//
// The analysis comes from the shell side (Get-STAnalysis in superterminal.ps1):
// PowerShell parses the line and resolves every command name with Get-Command,
// without running anything. That is "send it to PowerShell and see if it
// errors", minus the risk: an English sentence that starts with a real alias
// ("sort these by size", "del everything older than a week") never executes.
struct RouteDecision {
    bool to_ai = false;
    std::string line;    // the text to run or to send (prefixes removed)
    std::string reason;  // short, for logs and for Claude's context
};

RouteDecision decide_route(const std::string& line, const nlohmann::json& analysis);

// How much a list of words reads like English rather than command arguments.
int english_score(const std::string& text, int* word_count = nullptr);

}  // namespace st
