#include "brain.h"

#include "context.h"
#include "json_util.h"
#include "router.h"
#include "router_posix.h"
#include "safety.h"
#include "util.h"

using nlohmann::json;

namespace st {

namespace {
constexpr size_t kMemory = 6;         // earlier requests remembered for follow-ups
constexpr size_t kOutputTail = 3500;  // bytes of the last command's output that go along

const json kEmpty = json::object();
const json& sub(const json& j, const char* key) {
    return j.contains(key) && j[key].is_object() ? j[key] : kEmpty;
}

std::string confirm_note(const std::string& risk) {
    if (risk == "destructive") return "This deletes or overwrites. Enter to run, Esc to cancel.";
    if (risk == "modifies") return "This makes changes. Enter to run, Esc to cancel.";
    return "Enter to run, Esc to cancel.";
}
}  // namespace

std::string Brain::system_prompt(bool posix) {
    const std::string shell = posix ? "bash or zsh (named in the context)" : "PowerShell 7";
    std::string p =
        "You are the engine inside SuperTerminal, a terminal where people type either shell commands or plain "
        "English at the same prompt. The shell here is " + shell + ". SuperTerminal runs real commands itself. "
        "You only see a line when it isn't valid, names a command that doesn't exist, or reads like English. "
        "Work out what the person wants and reply with one JSON object.\n\n"
        "Pick a kind:\n"
        "- \"command\": a command for this shell that does what they asked. It runs in their live session, in "
        "their current folder, so session state (variables, location) carries over. Put the code in \"command\".\n"
        "- \"answer\": a question that's best answered in words (what something means, why the last command "
        "failed when no command would help, how to do something they didn't ask you to do now), or a request too "
        "ambiguous to act on safely. Put a short plain-text reply in \"answer\", a few lines at most, no markdown. "
        "You may suggest a command inside the answer. Write it the way a terminal's built-in help would: plain and "
        "direct, never about yourself. Don't mention AI, models, assistants or Claude, and don't say \"I\".\n"
        "- \"passthrough\": the line is already a valid command that they meant literally (for example \"echo the "
        "end\"). SuperTerminal then runs it unchanged.\n"
        "Fill fields that don't apply with an empty string, and set \"risk\" to \"read_only\" for answers and "
        "passthroughs.\n\n"
        "For \"command\", set \"risk\": \"read_only\" if it only reads or displays things (including changing the "
        "current folder); \"modifies\" if it creates, changes, moves, copies or renames files, changes settings, "
        "starts or installs things; \"destructive\" if it deletes, overwrites, kills processes, or can't easily be "
        "undone. SuperTerminal shows the person anything that isn't read-only before running it, so don't add "
        "confirmation flags. Be honest about risk: it decides whether the command runs without a check.\n\n"
        "Writing commands:\n";
    if (posix) {
        p += "- Write for the shell and OS in the context. On macOS the tools are the BSD versions (ls, sed, find, "
             "date, stat differ from GNU); on Linux they're GNU. Use only standard tools, ones the person named, or "
             "ones the context shows are installed.\n"
             "- PowerShell or cmd.exe habits (\"dir /w\", \"Get-ChildItem\", \"cls\", \"ipconfig\", \"type file\") "
             "mean: do what that would do, the way this shell does it.\n"
             "- Never use sudo unless they asked for it.\n";
    } else {
        p += "- Prefer built-in cmdlets; use other programs only if they're standard on this OS, the person named "
             "them, or the context shows they're installed.\n"
             "- Make the output readable: pick useful properties, show sizes in KB/MB/GB, sort sensibly, use "
             "Format-Table -AutoSize or Format-Wide where it helps. Don't pipe to Out-Host or Write-Host when plain "
             "output works. Use -LiteralPath for paths that contain [ or ].\n"
             "- cmd.exe, bash or Unix habits (\"dir /w\", \"ls -la\", \"rm -rf\", \"grep\", \"cat\", \"touch\") "
             "mean: do what that would do, the PowerShell way.\n";
    }
    p += "- People read the command before it runs and learn from it, so prefer the short, idiomatic version over "
         "elaborate formatting.\n"
         "- Use names from the folder listing rather than guessing.\n"
         "- Follow-ups (\"now sort them by size\", \"only the big ones\", \"do the same for pdfs\") build on the "
         "earlier requests shown in the context.\n"
         "- \"fix it\", \"try again\" or \"that didn't work\" refer to the last command, its error and output in "
         "the context.\n"
         "- Keep it to one line when that's natural; a short multi-line script is fine when it isn't.";
    return p;
}

const json& Brain::reply_schema() {
    static const json schema = {
        {"type", "object"},
        {"properties",
         {{"kind", {{"type", "string"}, {"enum", {"command", "answer", "passthrough"}}}},
          {"command", {{"type", "string"}}},
          {"risk", {{"type", "string"}, {"enum", {"read_only", "modifies", "destructive"}}}},
          {"answer", {{"type", "string"}}}}},
        {"required", {"kind", "command", "risk", "answer"}},
        {"additionalProperties", false}};
    return schema;
}

Brain::Brain(ClaudeClient& client, OutputLog& log, Settings settings, std::string session_shell)
    : client_(client), log_(log), settings_(std::move(settings)), session_shell_(std::move(session_shell)) {}

Settings Brain::settings_copy() {
    std::lock_guard lock(mu_);
    return settings_;
}

std::string Brain::shell_of(const json& req) const { return jstr(req, "shell", session_shell_); }

json Brain::handle(const json& req) {
    const std::string type = jstr(req, "type");
    if (type == "hello") return on_hello(req);
    if (type == "route") return on_route(req);
    if (type == "ask") return ask(req);
    if (type == "check") return on_check(req);
    if (type == "config") return on_config(req);
    if (type == "mark") {
        log_.mark_command_start();
        return {{"ok", true}};
    }
    return {{"kind", "error"}, {"message", "unknown request type '" + type + "'"}};
}

json Brain::on_hello(const json&) {
    Settings s = settings_copy();
    return {{"version", ST_VERSION},  {"model", s.model},
            {"effort", s.effort},     {"autorun", s.autorun},
            {"keyPresent", client_.has_key()}, {"capture", log_.enabled()}};
}

json Brain::on_route(const json& req) {
    const std::string line = jstr(req, "line");
    RouteDecision d = is_posix(shell_of(req))
                          ? decide_route_posix(line, jstr(req, "syntax", "ok"), parse_types(jstr(req, "types")))
                          : decide_route(line, sub(req, "analysis"));
    // Without a key, behave like the plain shell unless they explicitly asked.
    if (d.to_ai && !client_.has_key() && !starts_with(trim(line), "??")) {
        d.to_ai = false;
        d.reason = "no API key";
    }
    if (!d.to_ai) log_.mark_command_start();
    return {{"action", d.to_ai ? "ai" : "run"}, {"line", d.line}, {"reason", d.reason}};
}

json Brain::on_check(const json& req) {
    Settings s = settings_copy();
    AutorunVerdict v = check_autorun(sub(req, "analysis"), jstr(req, "risk"));
    bool autorun = s.autorun && v.allowed;
    if (autorun) log_.mark_command_start();
    return {{"autorun", autorun}, {"safe", v.allowed}, {"reason", s.autorun ? v.reason : "auto-run is off"}};
}

json Brain::on_config(const json& req) {
    const std::string key = jstr(req, "key");
    const std::string value = trim(jstr(req, "value"));
    std::lock_guard lock(mu_);
    std::string message;
    bool ok = true;
    if (key == "model") {
        std::string m = resolve_model(value);
        if (m.empty()) {
            ok = false;
            message = "Unknown model '" + value + "'. Use haiku, sonnet or opus.";
        } else {
            settings_.model = m;
            message = "Model set to " + m + ".";
        }
    } else if (key == "effort") {
        std::string e = to_lower(value);
        if (!valid_effort(e)) {
            ok = false;
            message = "Effort must be low, medium, high, xhigh or max.";
        } else {
            settings_.effort = e;
            message = "Effort set to " + e + ".";
        }
    } else if (key == "autorun") {
        std::string v = to_lower(value);
        if (v == "on" || v == "true" || v == "1") settings_.autorun = true;
        else if (v == "off" || v == "false" || v == "0") settings_.autorun = false;
        else ok = false;
        message = ok ? (settings_.autorun ? "Read-only commands now run straight away."
                                          : "Every translated command now waits for Enter.")
                     : "Use: st auto on | off";
    } else if (key == "shell") {
        settings_.shell = to_lower(value);
        message = settings_.shell.empty() ? "Default shell reset." : "New sessions will use " + settings_.shell + ".";
    } else if (!key.empty()) {
        ok = false;
        message = "Unknown setting '" + key + "'.";
    }
    if (ok && !key.empty() && !save_settings(settings_)) message += " (Couldn't save settings.json.)";
    return {{"ok", ok},
            {"message", message},
            {"model", settings_.model},
            {"effort", settings_.effort},
            {"autorun", settings_.autorun},
            {"shell", settings_.shell},
            {"keyPresent", client_.has_key()}};
}

std::string Brain::build_user_message(const json& req, bool posix) {
    const std::string cwd = jstr(req, "cwd");
    const std::string shell = shell_of(req);
    std::string m = "<context>\n";
    std::string os = jstr(req, "os");
    m += "os: " + (os.empty() ? os_description() : os) + "\n";
    m += "shell: " + (posix ? shell : "PowerShell") + " " + jstr(req, "shellVersion", jstr(req, "psVersion")) + "\n";
    m += "user home: " + jstr(req, "home", user_home()) + "\n";
    m += "current folder: " + cwd + "\n";
    std::string listing = jstr(req, "listing");
    if (!listing.empty()) m += "folder listing:\n" + tail(listing, 6000) + "\n";
    else if (!cwd.empty() && jbool(req, "isFileSystem", true)) m += "folder listing:\n" + folder_listing(cwd);

    if (req.contains("history") && req["history"].is_array() && !req["history"].empty()) {
        m += "recent commands (oldest first):\n";
        for (const auto& h : req["history"]) {
            std::string status = jbool(h, "ok", true) ? "ok" : "failed";
            if (h.contains("exit") && h["exit"].is_number_integer() && h["exit"].get<int>() != 0)
                status += ", exit code " + std::to_string(h["exit"].get<int>());
            m += "  [" + status + "] " + jstr(h, "line") + "\n";
            std::string err = jstr(h, "error");
            if (!err.empty()) m += "      error: " + tail(err, 600) + "\n";
        }
    } else if (req.contains("history") && req["history"].is_string() && !trim(jstr(req, "history")).empty()) {
        // bash/zsh send "status<TAB>command" lines.
        m += "recent commands (oldest first):\n";
        for (const auto& line : split_lines(jstr(req, "history"))) {
            size_t tab = line.find('\t');
            if (tab == std::string::npos) continue;
            std::string status = line.substr(0, tab);
            m += "  [" + (status == "0" ? std::string("ok") : "failed, exit code " + status) + "] " +
                 line.substr(tab + 1) + "\n";
        }
    }
    if (log_.enabled()) {
        std::string out = log_.since_last_command(kOutputTail);
        if (!trim(out).empty()) m += "screen since the last command ran (its output, then the current prompt):\n" + out;
    }
    {
        std::lock_guard lock(mu_);
        if (!memory_.empty()) {
            m += "earlier requests in this session (oldest first):\n";
            for (const auto& e : memory_) m += "  \"" + e.request + "\" -> " + e.kind + ": " + e.result + "\n";
        }
    }
    m += "</context>\n";
    std::string reason = jstr(req, "reason");
    if (!reason.empty()) m += "<why_you_see_this>" + reason + "</why_you_see_this>\n";
    m += "<typed>" + jstr(req, "request") + "</typed>";
    return m;
}

json Brain::ask(const json& req) {
    const std::string request = trim(jstr(req, "request"));
    if (request.empty()) return {{"kind", "error"}, {"message", "Nothing to ask."}};
    const bool posix = is_posix(shell_of(req));
    Settings s = settings_copy();
    const std::string user_message = build_user_message(req, posix);
    const std::string system = system_prompt(posix);
    // If the chosen model declines, try the other 5.5 models. Nothing outside
    // Haiku / Sonnet / Opus 5.5 is ever used.
    std::vector<std::string> models = {s.model};
    for (const char* m : {"claude-haiku-5-5", "claude-sonnet-5-5", "claude-opus-5-5"})
        if (s.model != m) models.push_back(m);
    ClaudeClient::Reply r;
    long long total_ms = 0;
    for (const auto& m : models) {
        r = client_.ask(m, s.effort, system, user_message, reply_schema());
        total_ms += r.elapsed_ms;
        if (r.ok || !r.refused) break;
    }
    r.elapsed_ms = total_ms;
    if (!r.ok) return {{"kind", "error"}, {"message", r.error}, {"elapsedMs", r.elapsed_ms}, {"model", s.model}};

    json out = r.result;
    std::string kind = jstr(out, "kind");
    if (kind != "command" && kind != "answer" && kind != "passthrough")
        return {{"kind", "error"}, {"message", "Got an unexpected reply. Try again."}};
    if (kind == "command" && trim(jstr(out, "command")).empty()) {
        kind = "answer";
        out["kind"] = kind;
        if (trim(jstr(out, "answer")).empty()) out["answer"] = "Couldn't work that one out. Try rephrasing it.";
    }
    // bash/zsh: decide auto-run here (PowerShell asks separately with its own parse).
    if (posix && kind == "command") {
        AutorunVerdict v = check_autorun_posix(jstr(out, "command"), jstr(out, "risk"));
        bool autorun = s.autorun && v.allowed;
        out["autorun"] = autorun;
        out["note"] = autorun ? "" : confirm_note(jstr(out, "risk"));
        if (autorun) log_.mark_command_start();
    }
    out["elapsedMs"] = r.elapsed_ms;
    out["model"] = r.model;
    out["inputTokens"] = r.input_tokens;
    out["outputTokens"] = r.output_tokens;

    std::lock_guard lock(mu_);
    std::string result = kind == "command" ? jstr(out, "command")
                         : kind == "answer" ? tail(jstr(out, "answer"), 300)
                                            : request;
    memory_.push_back({request, kind, result});
    while (memory_.size() > kMemory) memory_.pop_front();
    return out;
}

}  // namespace st
