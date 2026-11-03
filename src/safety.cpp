#include "safety.h"

#include <set>

#include "json_util.h"
#include "util.h"

using nlohmann::json;

namespace st {

namespace {

const std::set<std::string> kSafeVerbs = {"get",     "select", "sort",     "where",       "measure",
                                          "format",  "group",  "test",     "resolve",     "compare",
                                          "convertto", "convertfrom", "foreach", "split", "join"};

// Specific cmdlets that are safe although their verb isn't on the list above.
const std::set<std::string> kSafeCmdlets = {
    "out-string", "out-host",     "out-default",   "write-output", "write-host",  "set-location",
    "push-location", "pop-location", "clear-host", "get-help",     "select-string"};

// Programs that only read.
const std::set<std::string> kSafePrograms = {"whoami", "hostname", "ipconfig", "systeminfo", "tasklist",
                                             "where",  "findstr",  "tree",     "nslookup",   "ping",
                                             "ver",    "driverquery", "getmac",
                                             // pwsh on macOS / Linux runs the system tools
                                             "ls", "cat", "head", "tail", "du", "df", "wc", "uname", "date",
                                             "ps", "which", "file", "stat", "uptime", "sw_vers", "hostname"};

const std::set<std::string> kSafeGitSubcommands = {"status", "log",  "diff",     "show",    "ls-files",
                                                   "blame",  "rev-parse", "describe", "shortlog", "grep"};

const std::set<std::string> kSafeMethods = {
    "tostring", "toupper", "tolower", "toupperinvariant", "tolowerinvariant", "trim", "trimstart", "trimend",
    "split", "replace", "substring", "contains", "startswith", "endswith", "indexof", "lastindexof", "padleft",
    "padright", "join", "format", "round", "floor", "ceiling", "truncate", "abs", "max", "min", "pow", "sqrt",
    "getfilename", "getfilenamewithoutextension", "getextension", "getdirectoryname", "combine", "getfullpath",
    "equals", "compareto", "gettype", "toarray", "where", "foreach", "matches", "ismatch", "match",
    "getenvironmentvariable", "getfolderpath", "toshortdatestring", "tolongdatestring", "tolocaltime",
    "touniversaltime", "adddays", "addhours", "addminutes", "addseconds", "addmonths", "addyears", "subtract",
    "parse", "tryparse", "isnullorempty", "isnullorwhitespace", "getlogicaldrives", "getdrives", "now", "today"};

std::string program_name(std::string name) {
    name = to_lower(name);
    size_t slash = name.find_last_of("\\/");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    if (name.size() > 4 && name.substr(name.size() - 4) == ".exe") name.resize(name.size() - 4);
    return name;
}

}  // namespace

AutorunVerdict check_autorun(const json& analysis, const std::string& claude_risk) {
    AutorunVerdict v;
    if (claude_risk != "read_only") {
        v.reason = claude_risk == "destructive" ? "Claude marked it destructive" : "it changes things";
        return v;
    }
    if (analysis.contains("parseErrors") && !analysis["parseErrors"].empty()) {
        v.reason = "PowerShell could not parse it";
        return v;
    }
    if (jint(analysis, "redirections", 0) > 0) {
        v.reason = "it writes output to a file";
        return v;
    }
    if (analysis.contains("members") && analysis["members"].is_array()) {
        for (const auto& m : analysis["members"]) {
            std::string name = to_lower(m.is_string() ? m.get<std::string>() : std::string());
            if (!kSafeMethods.count(name)) {
                v.reason = "it calls the ." + (name.empty() ? std::string("?") : name) + "() method";
                return v;
            }
        }
    }
    if (!analysis.contains("commands") || !analysis["commands"].is_array()) {
        v.reason = "no analysis";
        return v;
    }
    for (const auto& c : analysis["commands"]) {
        const std::string name = jstr(c, "name");
        const std::string kind = jstr(c, "kind");
        std::string rkind = jstr(c, "resolvedKind");
        if (rkind.empty()) rkind = kind;
        const std::string resolved = to_lower(jstr(c, "resolved", name));
        const std::string verb = to_lower(jstr(c, "verb"));

        if (kind == "NotFound" || kind == "dynamic" || name.empty()) {
            v.reason = "it runs a command PowerShell can't identify";
            return v;
        }
        if (rkind == "Cmdlet" || rkind == "Function") {
            if (kSafeCmdlets.count(resolved)) continue;
            if (!verb.empty() && kSafeVerbs.count(verb)) continue;
            v.reason = "it runs " + jstr(c, "resolved", name);
            return v;
        }
        if (rkind == "Application") {
            std::string prog = program_name(jstr(c, "resolved", name));
            if (kSafePrograms.count(prog)) continue;
            if (prog == "git") {
                const json& words = c.contains("barewords") ? c["barewords"] : json::array();
                std::string sub = words.empty() || !words[0].is_string() ? "" : to_lower(words[0].get<std::string>());
                if (kSafeGitSubcommands.count(sub)) continue;
                if ((sub == "branch" || sub == "tag" || sub == "remote") && words.size() == 1) continue;
                v.reason = "it runs git " + sub;
                return v;
            }
            v.reason = "it runs " + prog;
            return v;
        }
        v.reason = "it runs " + jstr(c, "resolved", name) + " (" + rkind + ")";
        return v;
    }
    v.allowed = true;
    v.reason = "read-only";
    return v;
}

}  // namespace st
