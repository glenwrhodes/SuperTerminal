// SuperTerminal: a shell that also understands plain English.
//
// st runs your real shell (PowerShell, bash, zsh, or bash inside WSL) inside a
// pseudo terminal and relays it to this window. A small integration script,
// compiled in, hooks the Enter key: every line is checked by the shell itself
// (parser and command lookup, nothing runs), and anything that isn't a
// runnable command - or reads like English - is turned into one.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <iostream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "brain.h"
#include "claude_client.h"
#include "ipc.h"
#include "json_util.h"
#include "output_log.h"
#include "pty_host.h"
#include "router.h"
#include "router_posix.h"
#include "safety.h"
#include "scripts.h"
#include "settings.h"
#include "util.h"

#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

using nlohmann::json;
using namespace st;
namespace fs = std::filesystem;

namespace {

void print_help() {
    std::printf(
        "SuperTerminal %s - your shell, plus plain English.\n\n"
        "Usage: st [options]\n\n"
        "  (no options)            Start a session in your default shell.\n"
        "  --shell <name>          pwsh, powershell, wsl, wsl:<distro>, bash or zsh (or a path to one).\n"
        "                          Default: pwsh on Windows, your login shell on macOS/Linux.\n"
        "  --direct                Run the shell directly in this terminal (no output capture).\n"
        "  --ask \"<request>\"       Translate one request and print the JSON reply (no session).\n"
        "     --model <m>          ...with haiku, sonnet or opus.\n"
        "     --effort <e>         ...with low, medium, high, xhigh or max.\n"
#ifdef _WIN32
        "  --install-terminal-profile    Add SuperTerminal to Windows Terminal's profile list.\n"
        "  --uninstall-terminal-profile  Remove it again.\n"
        "  --vt-input              Send keys as plain VT text instead of win32-input-mode.\n"
#endif
        "  --version               Print the version.\n\n"
        "Needs the ANTHROPIC_API_KEY environment variable for plain-English requests.\n"
        "Inside a session, type `st help`.\n",
        ST_VERSION);
}

// ------------------------------------------------------------------ shells

struct ShellPlan {
    std::string kind;    // pwsh | powershell | wsl | bash | zsh
    std::string exe;     // full path, or a name found on PATH
    std::string distro;  // wsl only
    std::string error;
};

std::string find_in_path(const std::string& name) {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    std::wstring n = widen(name);
    if (SearchPathW(nullptr, n.c_str(), L".exe", MAX_PATH, buf, nullptr)) return narrow(buf);
    return {};
#else
    if (name.find('/') != std::string::npos) return access(name.c_str(), X_OK) == 0 ? name : std::string();
    std::string path = read_env("PATH");
    size_t start = 0;
    while (start <= path.size()) {
        size_t colon = path.find(':', start);
        if (colon == std::string::npos) colon = path.size();
        std::string dir = path.substr(start, colon - start);
        if (!dir.empty()) {
            std::string full = dir + "/" + name;
            if (access(full.c_str(), X_OK) == 0) return full;
        }
        start = colon + 1;
    }
    return {};
#endif
}

ShellPlan resolve_shell(std::string requested) {
    ShellPlan p;
    std::string req = to_lower(trim(requested));
    std::string base = req;
    size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    if (base.size() > 4 && base.substr(base.size() - 4) == ".exe") base.resize(base.size() - 4);

#ifdef _WIN32
    if (req.empty()) {
        p.exe = find_in_path("pwsh");
        if (p.exe.empty()) {
            std::string pf = read_env("ProgramW6432");
            std::string guess = (pf.empty() ? std::string("C:\\Program Files") : pf) + "\\PowerShell\\7\\pwsh.exe";
            if (GetFileAttributesW(widen(guess).c_str()) != INVALID_FILE_ATTRIBUTES) p.exe = guess;
        }
        p.kind = p.exe.empty() ? "powershell" : "pwsh";
        if (p.exe.empty()) p.exe = find_in_path("powershell");
        return p;
    }
    if (base == "pwsh" || base == "powershell") {
        p.kind = base;
        p.exe = slash != std::string::npos ? requested : find_in_path(base);
        if (p.exe.empty()) p.error = base + " isn't installed or isn't on PATH.";
        return p;
    }
    if (base == "wsl" || starts_with(base, "wsl:") || base == "bash" || base == "zsh") {
        p.kind = "wsl";
        if (starts_with(req, "wsl:")) p.distro = trim(requested.substr(4));
        p.exe = find_in_path("wsl");
        if (p.exe.empty()) p.error = "WSL isn't installed (wsl.exe wasn't found). Install it with: wsl --install";
        return p;
    }
    if (base == "cmd") {
        p.error = "cmd.exe isn't supported yet: it has no way to hook the Enter key. Use --shell pwsh or --shell wsl.";
        return p;
    }
    p.error = "Unknown shell '" + requested + "'. Use pwsh, powershell, wsl, wsl:<distro>, bash or zsh.";
    return p;
#else
    if (req.empty()) {
        std::string login = read_env("SHELL");
        std::string b = login.substr(login.find_last_of('/') + 1);
        if (b == "bash" || b == "zsh" || b == "pwsh") {
            p.kind = b;
            p.exe = login;
            return p;
        }
        req = base = "bash";
    }
    if (base == "bash" || base == "zsh" || base == "pwsh") {
        p.kind = base;
        p.exe = req.find('/') != std::string::npos ? requested : find_in_path(base);
        if (p.exe.empty()) p.error = base + " isn't installed or isn't on PATH.";
        return p;
    }
    p.error = "Unknown shell '" + requested + "'. Use bash, zsh or pwsh.";
    return p;
#endif
}

std::string ps_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) out += c == '\'' ? std::string("''") : std::string(1, c);
    return out + "'";
}

// ------------------------------------------------------------------ client mode
// The bash/zsh hooks call `st --client <pipe> <type>` with fields on stdin as
// key \x1f value \x1e ..., and read a few plain lines back.

int run_client(const std::string& pipe, const std::string& type) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::string in((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
    json req = {{"type", type}};
    size_t pos = 0;
    while (pos < in.size()) {
        size_t rs = in.find('\x1e', pos);
        if (rs == std::string::npos) rs = in.size();
        std::string field = in.substr(pos, rs - pos);
        size_t us = field.find('\x1f');
        if (us != std::string::npos) req[field.substr(0, us)] = field.substr(us + 1);
        pos = rs + 1;
    }
    std::string error;
    std::string line = ipc_request(pipe, req.dump(-1, ' ', false, json::error_handler_t::replace), &error);
    json reply = line.empty() ? json() : json::parse(line, nullptr, false);
    std::string out;
    if (!reply.is_object()) {
        if (type == "ask") out = "error\n0\n\n" + (error.empty() ? std::string("No reply.") : error);
        else if (type == "config") out = "  " + (error.empty() ? std::string("No reply.") : error) + "\n";
        std::fwrite(out.data(), 1, out.size(), stdout);
        return 1;
    }
    if (type == "route") {
        out = jstr(reply, "action", "run") + "\n" + jstr(reply, "reason") + "\n" + jstr(reply, "line");
    } else if (type == "ask") {
        std::string kind = jstr(reply, "kind", "error");
        std::string payload = kind == "command" ? jstr(reply, "command")
                              : kind == "answer" ? jstr(reply, "answer")
                              : kind == "error"  ? jstr(reply, "message")
                                                 : "";
        out = kind + "\n" + (jbool(reply, "autorun") ? "1" : "0") + "\n" + jstr(reply, "note") + "\n" + payload;
    } else if (type == "hello") {
        out = jstr(reply, "version") + "\n" + (jbool(reply, "keyPresent") ? "1" : "0") + "\n";
    } else if (type == "config") {
        if (!jstr(reply, "message").empty()) out += "  " + jstr(reply, "message") + "\n";
        out += "    model   " + jstr(reply, "model") + "\n";
        out += "    effort  " + jstr(reply, "effort") + "\n";
        out += std::string("    auto    ") +
               (jbool(reply, "autorun") ? "on (read-only commands run straight away)"
                                        : "off (every command waits for Enter)") + "\n";
        out += std::string("    key     ANTHROPIC_API_KEY is ") + (jbool(reply, "keyPresent") ? "set" : "NOT SET") + "\n";
    }
    std::fwrite(out.data(), 1, out.size(), stdout);
    return 0;
}

// ------------------------------------------------------------------ test hooks

int run_line_test(const std::string& mode) {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (trim(line).empty()) continue;
        json in = json::parse(line, nullptr, false);
        if (!in.is_object()) {
            std::cout << json{{"error", "bad json"}}.dump() << "\n";
            continue;
        }
        const json analysis = in.contains("analysis") && in["analysis"].is_object() ? in["analysis"] : json::object();
        if (mode == "safety") {
            AutorunVerdict v = check_autorun(analysis, jstr(in, "risk"));
            std::cout << json{{"allowed", v.allowed}, {"reason", v.reason}}.dump() << "\n";
        } else if (mode == "safety-posix") {
            AutorunVerdict v = check_autorun_posix(jstr(in, "command"), jstr(in, "risk"));
            std::cout << json{{"allowed", v.allowed}, {"reason", v.reason}}.dump() << "\n";
        } else if (mode == "route-posix") {
            std::map<std::string, std::string> types;
            if (in.contains("types") && in["types"].is_object())
                for (auto& [k, v] : in["types"].items())
                    if (v.is_string()) types[k] = v.get<std::string>();
            RouteDecision d = decide_route_posix(jstr(in, "line"), jstr(in, "syntax", "ok"), types);
            std::cout << json{{"line", d.line}, {"toAi", d.to_ai}, {"reason", d.reason}}.dump() << "\n";
        } else {
            RouteDecision d = decide_route(jstr(in, "line"), analysis);
            std::cout << json{{"line", d.line}, {"toAi", d.to_ai}, {"reason", d.reason}}.dump() << "\n";
        }
    }
    return 0;
}

int run_ask(const std::string& request, const std::string& model, const std::string& effort,
            const std::string& shell) {
    Settings settings = load_settings();
    if (!model.empty()) {
        settings.model = resolve_model(model);
        if (settings.model.empty()) {
            std::fprintf(stderr, "Unknown model '%s'. Use haiku, sonnet or opus.\n", model.c_str());
            return 2;
        }
    }
    if (!effort.empty()) settings.effort = effort;
    ClaudeClient client;
    OutputLog log;
    std::string sh = shell.empty() ? resolve_shell(settings.shell).kind : resolve_shell(shell).kind;
    if (sh == "wsl") sh = "bash";
    Brain brain(client, log, settings, sh);
    json req = {{"type", "ask"}, {"request", request}, {"cwd", path_to_utf8(fs::current_path())}};
    json reply = brain.handle(req);
    std::cout << reply.dump(2, ' ', false, json::error_handler_t::replace) << std::endl;
    return jstr(reply, "kind") == "error" ? 1 : 0;
}

#ifdef _WIN32
fs::path terminal_fragment_path() {
    std::string local = read_env("LOCALAPPDATA");
    return path_from_utf8(local) / "Microsoft" / "Windows Terminal" / "Fragments" / "SuperTerminal" /
           "superterminal.json";
}

int install_terminal_profile(bool install) {
    fs::path file = terminal_fragment_path();
    std::error_code ec;
    if (!install) {
        fs::remove_all(file.parent_path(), ec);
        std::printf("Removed the SuperTerminal profiles from Windows Terminal.\n");
        return 0;
    }
    std::string exe = self_exe_path();
    json profiles = json::array();
    profiles.push_back({{"name", "SuperTerminal"},
                        {"commandline", "\"" + exe + "\" --shell pwsh"},
                        {"startingDirectory", "%USERPROFILE%"}});
    if (!find_in_path("wsl").empty())
        profiles.push_back({{"name", "SuperTerminal (WSL)"},
                            {"commandline", "\"" + exe + "\" --shell wsl"},
                            {"startingDirectory", "%USERPROFILE%"}});
    fs::create_directories(file.parent_path(), ec);
    std::ofstream f(file, std::ios::trunc);
    f << json{{"profiles", profiles}}.dump(2) << "\n";
    if (!f) {
        std::fprintf(stderr, "Couldn't write %s\n", path_to_utf8(file).c_str());
        return 1;
    }
    std::printf("Added %zu profile(s) to Windows Terminal. Open a new tab's dropdown to find them.\n"
                "(They point at %s; run this again if you move st.exe.)\n",
                profiles.size(), exe.c_str());
    return 0;
}
#endif

[[noreturn]] void finish(int code, IpcServer* server) {
    std::fflush(stdout);
    if (server) server->stop();
    // Skip destructors: background threads (IPC, warm-up) may still be running.
#ifdef _WIN32
    ExitProcess(static_cast<UINT>(code));
#else
    _exit(code);
#endif
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** wargv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(narrow(wargv[i]));
#else
int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
#endif
    bool direct = false, ask = false;
#ifdef _WIN32
    bool win32_input = true;
#endif
    std::string shell_arg, ask_text, ask_model, ask_effort;

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto next = [&]() -> std::string { return i + 1 < args.size() ? args[++i] : std::string(); };
        if (a == "--client") {
            std::string pipe = next(), type = next();
            return run_client(pipe, type);
        }
        if (a == "--direct") direct = true;
        else if (a == "--shell") shell_arg = next();
        else if (a == "--ask") ask = true, ask_text = next();
        else if (a == "--model") ask_model = next();
        else if (a == "--effort") ask_effort = next();
        else if (a == "--route-test") return run_line_test("route");
        else if (a == "--route-test-posix") return run_line_test("route-posix");
        else if (a == "--safety-test") return run_line_test("safety");
        else if (a == "--safety-test-posix") return run_line_test("safety-posix");
#ifdef _WIN32
        else if (a == "--vt-input") win32_input = false;
        else if (a == "--install-terminal-profile") return install_terminal_profile(true);
        else if (a == "--uninstall-terminal-profile") return install_terminal_profile(false);
#endif
        else if (a == "--version") return std::printf("SuperTerminal %s\n", ST_VERSION), 0;
        else if (a == "--help" || a == "-h" || a == "/?") return print_help(), 0;
        else {
            std::fprintf(stderr, "Unknown option: %s (try --help)\n", a.c_str());
            return 2;
        }
    }
    if (ask) return run_ask(ask_text, ask_model, ask_effort, shell_arg);

    Settings settings = load_settings();
    ShellPlan shell = resolve_shell(shell_arg.empty() ? settings.shell : shell_arg);
    if (!shell.error.empty()) {
        std::fprintf(stderr, "SuperTerminal: %s\n", shell.error.c_str());
        return 2;
    }
    const std::string session_shell = shell.kind == "wsl" ? "bash" : shell.kind;

    ClaudeClient client;
    OutputLog log;
    Brain brain(client, log, settings, session_shell);
    IpcServer server([&brain](const json& req) { return brain.handle(req); });
    const bool ipc_ok = server.start();
    if (!ipc_ok) std::fprintf(stderr, "SuperTerminal: couldn't open its local socket; starting the plain shell.\n");

    set_env("SUPERTERMINAL", "1");
    set_env("SUPERTERMINAL_VERSION", ST_VERSION);
    set_env("SUPERTERMINAL_SHELL", session_shell);
    set_env("SUPERTERMINAL_EXE", self_exe_path());
    if (ipc_ok) set_env("SUPERTERMINAL_PIPE", server.name());

    std::string err;
    HostOptions host;
    host.log = &log;
#ifdef _WIN32
    host.win32_input = win32_input;
    if (shell.kind == "wsl") {
        fs::path hook = install_script("superterminal.bash", &err);
        if (hook.empty()) std::fprintf(stderr, "SuperTerminal: %s\n", err.c_str());
        set_env("SUPERTERMINAL_HOOK", path_to_utf8(hook));
        // Pass our variables into Linux; /p translates Windows paths to /mnt/... paths.
        std::string wslenv = read_env("WSLENV");
        set_env("WSLENV", (wslenv.empty() ? "" : wslenv + ":") +
                              "SUPERTERMINAL:SUPERTERMINAL_VERSION:SUPERTERMINAL_SHELL:SUPERTERMINAL_PIPE:"
                              "SUPERTERMINAL_EXE/p:SUPERTERMINAL_HOOK/p");
        std::string cmd = "\"" + shell.exe + "\"";
        if (!shell.distro.empty()) cmd += " -d \"" + shell.distro + "\"";
        cmd += hook.empty() ? std::string() : " -e bash -c \"exec bash --rcfile \\\"$SUPERTERMINAL_HOOK\\\" -i\"";
        host.command_line = widen(cmd);
    } else {
        fs::path script = install_script("superterminal.ps1", &err);
        std::string cmd = "\"" + shell.exe + "\" -NoLogo -NoExit";
        if (!script.empty())
            cmd += " -Command \". ([scriptblock]::Create([IO.File]::ReadAllText(" + ps_quote(path_to_utf8(script)) +
                   ")))\"";
        else
            std::fprintf(stderr, "SuperTerminal: %s; starting plain PowerShell.\n", err.c_str());
        host.command_line = widen(cmd);
    }
#else
    if (shell.kind == "bash") {
        fs::path hook = install_script("superterminal.bash", &err);
        host.argv = {shell.exe};
        if (!hook.empty()) host.argv.insert(host.argv.end(), {"--rcfile", hook.string()});
        host.argv.push_back("-i");
    } else if (shell.kind == "zsh") {
        fs::path hook = install_script("superterminal.zsh", &err);
        fs::path zdot = hook.empty() ? fs::path() : prepare_zdotdir(hook, &err);
        if (!zdot.empty()) {
            std::string user_zdot = read_env("ZDOTDIR");
            set_env("SUPERTERMINAL_USER_ZDOTDIR", user_zdot.empty() ? read_env("HOME") : user_zdot);
            set_env("ZDOTDIR", zdot.string());
        }
        host.argv = {shell.exe, "-i"};
    } else {  // pwsh
        fs::path script = install_script("superterminal.ps1", &err);
        host.argv = {shell.exe, "-NoLogo", "-NoExit"};
        if (!script.empty())
            host.argv.insert(host.argv.end(),
                             {"-Command", ". ([scriptblock]::Create([IO.File]::ReadAllText(" +
                                              ps_quote(script.string()) + ")))"});
    }
    if (!err.empty()) std::fprintf(stderr, "SuperTerminal: %s; starting the plain shell.\n", err.c_str());
#endif

    // Open the HTTPS connection early so the first request doesn't pay for the TLS handshake.
    if (client.has_key()) std::thread([&client, m = settings.model] { client.warm_up(m); }).detach();

    int code;
    if (direct || !has_console()) {
        code = run_direct(host);
    } else {
        log.set_enabled(true);
        code = run_in_pseudoconsole(host);
        if (code == -1) {
            log.set_enabled(false);
            code = run_direct(host);
        }
    }
    finish(code, &server);
}
