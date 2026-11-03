#pragma once
#include <deque>
#include <map>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "claude_client.h"
#include "output_log.h"
#include "settings.h"

namespace st {

// Answers the requests the shell integration sends:
//   hello   - startup info for the banner
//   route   - run this line as-is, or translate it?
//   ask     - turn an English request into a command (or an answer)
//   check   - may this command run without Enter? (PowerShell; bash/zsh get it with "ask")
//   mark    - a command is about to run (for output capture)
//   config  - st model / st effort / st auto
// A request's "shell" field ("pwsh", "powershell", "bash", "zsh") picks the
// PowerShell or the POSIX rules; it defaults to the session's shell.
class Brain {
public:
    Brain(ClaudeClient& client, OutputLog& log, Settings settings, std::string session_shell);

    nlohmann::json handle(const nlohmann::json& request);
    nlohmann::json ask(const nlohmann::json& request);

    static const nlohmann::json& reply_schema();
    static std::string system_prompt(bool posix);
    static bool is_posix(const std::string& shell) { return shell == "bash" || shell == "zsh" || shell == "sh"; }

private:
    nlohmann::json on_hello(const nlohmann::json& req);
    nlohmann::json on_route(const nlohmann::json& req);
    nlohmann::json on_check(const nlohmann::json& req);
    nlohmann::json on_config(const nlohmann::json& req);
    std::string build_user_message(const nlohmann::json& req, bool posix);
    std::string shell_of(const nlohmann::json& req) const;
    Settings settings_copy();

    struct Exchange {
        std::string request, kind, result;
    };

    ClaudeClient& client_;
    OutputLog& log_;
    std::mutex mu_;
    Settings settings_;
    std::string session_shell_;
    std::deque<Exchange> memory_;
};

}  // namespace st
