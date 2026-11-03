#pragma once
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "router.h"
#include "safety.h"

namespace st {

// A rough parse of a bash/zsh line: enough to find each simple command's
// name and arguments, redirections and substitutions. Not a full shell parser.
struct PosixCommand {
    std::string name;
    std::vector<std::string> args;
    std::vector<bool> quoted;  // per arg: had quotes (so it's data, not English)
};
struct PosixParse {
    std::vector<PosixCommand> commands;
    bool file_redirect = false;  // > file, >> file (not >/dev/null or 2>&1)
    bool substitution = false;   // $(...) or `...`
    bool background = false;     // trailing &
};
PosixParse parse_posix(std::string_view line);

// "word\ttype" lines from the shell (type -t / whence -w): alias, function,
// builtin, keyword, file/command, or none.
std::map<std::string, std::string> parse_types(const std::string& text);

// syntax: "ok", "incomplete" (the shell wants more lines) or "error" (from bash -n / zsh -n).
RouteDecision decide_route_posix(const std::string& line, const std::string& syntax,
                                 const std::map<std::string, std::string>& types);

// The bash/zsh version of check_autorun: read-only programs only.
AutorunVerdict check_autorun_posix(const std::string& command, const std::string& claude_risk);

}  // namespace st
