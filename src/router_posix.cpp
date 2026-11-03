#include "router_posix.h"

#include <cctype>
#include <set>

#include "util.h"

namespace st {

namespace {

bool is_assignment(const std::string& w) {
    size_t eq = w.find('=');
    if (eq == std::string::npos || eq == 0) return false;
    if (!(std::isalpha(static_cast<unsigned char>(w[0])) || w[0] == '_')) return false;
    for (size_t i = 1; i < eq; ++i)
        if (!(std::isalnum(static_cast<unsigned char>(w[i])) || w[i] == '_')) return false;
    return true;
}

std::string base_name(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

PosixParse parse_posix(std::string_view s) {
    PosixParse out;
    std::vector<std::string> words;
    std::vector<bool> quoted;
    std::string cur;
    bool cur_quoted = false, in_word = false;

    auto end_word = [&] {
        if (in_word) {
            words.push_back(cur);
            quoted.push_back(cur_quoted);
        }
        cur.clear();
        cur_quoted = in_word = false;
    };
    auto end_command = [&] {
        end_word();
        size_t i = 0;
        while (i < words.size() && !quoted[i] && is_assignment(words[i])) ++i;  // FOO=1 cmd
        if (i < words.size()) {
            PosixCommand c;
            c.name = words[i];
            for (size_t j = i + 1; j < words.size(); ++j) {
                c.args.push_back(words[j]);
                c.quoted.push_back(quoted[j]);
            }
            out.commands.push_back(std::move(c));
        }
        words.clear();
        quoted.clear();
    };

    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        char c = s[i];
        if (c == '\\' && i + 1 < n) {
            cur += s[i + 1];
            in_word = true;
            i += 2;
            continue;
        }
        if (c == '\'') {
            in_word = cur_quoted = true;
            size_t j = s.find('\'', i + 1);
            if (j == std::string_view::npos) j = n;
            cur.append(s.substr(i + 1, j - i - 1));
            i = j + 1;
            continue;
        }
        if (c == '"') {
            in_word = cur_quoted = true;
            size_t j = i + 1;
            while (j < n && s[j] != '"') {
                if (s[j] == '\\' && j + 1 < n) {
                    cur += s[j + 1];
                    j += 2;
                    continue;
                }
                if (s[j] == '`' || (s[j] == '$' && j + 1 < n && s[j + 1] == '(')) out.substitution = true;
                cur += s[j++];
            }
            i = j + 1;
            continue;
        }
        if (c == '`' || (c == '$' && i + 1 < n && s[i + 1] == '(')) out.substitution = true;
        if (c == '#' && !in_word) {  // comment to end of line
            while (i < n && s[i] != '\n') ++i;
            continue;
        }
        if (c == ' ' || c == '\t') {
            end_word();
            ++i;
            continue;
        }
        if (c == '\n' || c == ';' || c == '|' || c == '(' || c == ')') {
            end_command();
            ++i;
            continue;
        }
        if (c == '&') {
            if (i + 1 < n && s[i + 1] == '&') {
                end_command();
                i += 2;
                continue;
            }
            if (i + 1 < n && s[i + 1] == '>') {  // &> file
                end_word();
                ++i;
                c = '>';
            } else {
                end_command();
                if (trim(s.substr(i + 1)).empty()) out.background = true;
                ++i;
                continue;
            }
        }
        if (c == '>' || c == '<') {
            // A bare number right before is a file descriptor (2>), not an argument.
            bool fd_prefix = in_word && !cur_quoted && !cur.empty() &&
                             cur.find_first_not_of("0123456789") == std::string::npos;
            if (fd_prefix) cur.clear(), in_word = false;
            end_word();
            const bool output = c == '>';
            ++i;
            while (i < n && (s[i] == '>' || s[i] == '|')) ++i;
            bool to_fd = false;
            if (i < n && s[i] == '&') to_fd = true, ++i;
            while (i < n && (s[i] == ' ' || s[i] == '\t')) ++i;
            std::string target;
            while (i < n && !std::isspace(static_cast<unsigned char>(s[i])) && s[i] != ';' && s[i] != '|' &&
                   s[i] != '&')
                target += s[i++];
            if (output && !to_fd && target != "/dev/null" && !target.empty()) out.file_redirect = true;
            continue;
        }
        cur += c;
        in_word = true;
        ++i;
    }
    end_command();
    return out;
}

std::map<std::string, std::string> parse_types(const std::string& text) {
    std::map<std::string, std::string> m;
    for (const auto& line : split_lines(text)) {
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        m[line.substr(0, tab)] = trim(line.substr(tab + 1));
    }
    return m;
}

RouteDecision decide_route_posix(const std::string& input, const std::string& syntax,
                                 const std::map<std::string, std::string>& types) {
    RouteDecision d;
    std::string line = trim(input);
    d.line = line;
    if (starts_with(line, "!!")) {
        d.line = trim(line.substr(2));
        d.reason = "forced: run as typed";
        return d;
    }
    if (starts_with(line, "??")) {
        d.to_ai = true;
        d.line = trim(line.substr(2));
        d.reason = "forced: treat as plain English";
        return d;
    }
    int words = 0;
    const int raw_score = english_score(line, &words);
    if (line.size() > 1 && line.back() == '?' && words >= 2) {
        d.to_ai = true;
        d.reason = "it reads as a question";
        return d;
    }
    if (syntax == "error") {
        d.to_ai = true;
        d.reason = "it is not valid shell syntax";
        return d;
    }
    if (syntax == "incomplete") {
        if (raw_score >= 2 && words >= 2) {
            d.to_ai = true;
            d.reason = "it is English with an unclosed quote";
            return d;
        }
        d.reason = "incomplete input; the shell continues on the next line";
        return d;
    }

    PosixParse p = parse_posix(line);
    if (p.commands.empty()) {
        d.reason = "nothing to look up";
        return d;
    }
    for (const auto& c : p.commands) {
        auto it = types.find(c.name);
        if (it != types.end() && (it->second.empty() || it->second == "none")) {
            d.to_ai = true;
            d.reason = "'" + c.name + "' is not a command this shell knows";
            return d;
        }
    }

    int score = 0, bare = 0, params = 0;
    for (const auto& c : p.commands) {
        for (size_t i = 0; i < c.args.size(); ++i) {
            const std::string& a = c.args[i];
            if (c.quoted[i]) continue;
            if (starts_with(a, "-")) {
                ++params;
                continue;
            }
            ++bare;
            int w = 0;
            score += english_score(a, &w);
            // cmd.exe habits: "dir /w" (GNU dir exists, but /w is meant as a switch)
            if (c.name == "dir" && a.size() >= 2 && a.size() <= 4 && a[0] == '/' &&
                std::isalpha(static_cast<unsigned char>(a[1]))) {
                d.to_ai = true;
                d.reason = "'dir " + a + "' is cmd.exe syntax";
                return d;
            }
        }
    }
    bool codey = params > 0 || p.commands.size() > 1 || p.file_redirect || p.substitution ||
                 line.find('$') != std::string::npos;
    if ((!codey && score >= 2 && bare >= 2) || (codey && score >= 5)) {
        d.to_ai = true;
        d.reason = "it reads like English, though '" + p.commands[0].name + "' is a command";
        return d;
    }
    d.reason = "valid shell command";
    return d;
}

AutorunVerdict check_autorun_posix(const std::string& command, const std::string& claude_risk) {
    static const std::set<std::string> kSafe = {
        "ls", "ll", "la", "find", "cat", "head", "tail", "less", "more", "grep", "egrep", "fgrep", "rg", "ag", "fd",
        "du", "df", "wc", "sort", "uniq", "cut", "column", "pwd", "echo", "printf", "which", "type", "whereis",
        "file", "stat", "tree", "uname", "whoami", "id", "date", "cal", "env", "printenv", "hostname", "uptime",
        "free", "lsblk", "lscpu", "lsusb", "lspci", "ps", "pgrep", "jq", "basename", "dirname", "realpath",
        "readlink", "history", "cd", "pushd", "popd", "true", "sw_vers", "system_profiler", "mdfind", "mdls",
        "netstat", "ss", "lsof", "nproc", "arch", "locale", "groups", "w", "who", "dig", "nslookup", "host",
        "ping", "git", "tr", "nl", "md5sum", "sha256sum", "shasum", "numfmt", "seq", "test", "[", "getconf"};
    static const std::set<std::string> kGitSafe = {"status", "log",      "diff",     "show",    "ls-files",
                                                   "blame",  "rev-parse", "describe", "shortlog", "grep"};
    static const std::set<std::string> kFindUnsafe = {"-delete", "-exec", "-execdir", "-ok", "-okdir",
                                                      "-fprint", "-fprint0", "-fprintf", "-fls"};
    AutorunVerdict v;
    if (claude_risk != "read_only") {
        v.reason = claude_risk == "destructive" ? "marked destructive" : "it changes things";
        return v;
    }
    PosixParse p = parse_posix(command);
    if (p.file_redirect) return v.reason = "it writes output to a file", v;
    if (p.substitution) return v.reason = "it runs a command substitution", v;
    if (p.background) return v.reason = "it runs in the background", v;
    if (p.commands.empty()) return v.reason = "nothing to run", v;
    for (const auto& c : p.commands) {
        std::string name = base_name(c.name);
        if (!kSafe.count(name)) return v.reason = "it runs " + name, v;
        if (name == "find")
            for (const auto& a : c.args)
                if (kFindUnsafe.count(a)) return v.reason = "find " + a + " changes things", v;
        if (name == "ping") {  // Unix ping runs forever without -c
            bool counted = false;
            for (const auto& a : c.args)
                if (starts_with(a, "-c")) counted = true;
            if (!counted) return v.reason = "ping without -c never stops", v;
        }
        if (name == "git") {
            std::string sub;
            size_t nonflag = 0;
            for (const auto& a : c.args)
                if (!starts_with(a, "-")) {
                    if (sub.empty()) sub = a;
                    ++nonflag;
                }
            if (kGitSafe.count(sub)) continue;
            if ((sub == "branch" || sub == "tag" || sub == "remote") && nonflag == 1) continue;
            return v.reason = "it runs git " + sub, v;
        }
    }
    v.allowed = true;
    v.reason = "read-only";
    return v;
}

}  // namespace st
