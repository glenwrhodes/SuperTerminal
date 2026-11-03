#include "router.h"

#include <cctype>
#include <set>

#include "json_util.h"
#include "util.h"

using nlohmann::json;

namespace st {

namespace {

// Words that almost never appear as bare (unquoted) command arguments but are
// everywhere in English requests.
const std::set<std::string> kStrong = {
    "the",   "a",     "an",    "all",   "my",    "me",     "i",      "i'm",    "you",   "your",  "these",
    "those", "this",  "that",  "them",  "it",    "its",    "it's",   "please", "every", "some",  "any",
    "which", "what",  "what's", "how",  "why",   "who",    "is",     "are",    "was",   "were",  "be",
    "can",   "could", "would", "should", "will", "do",     "does",   "did",    "of",    "than",  "there",
    "here",  "want",  "need",  "let's", "lets",  "where's", "whats",  "into",   "onto",  "about", "between"};

// Words that lean English but are plausible arguments on their own.
const std::set<std::string> kWeak = {
    "files",   "file",     "folder",  "folders",  "directory", "directories", "everything", "anything",
    "biggest", "largest",  "smallest", "newest",  "oldest",    "recent",      "recently",   "older",
    "newer",   "bigger",   "larger",  "smaller",  "last",      "today",       "yesterday",  "week",
    "month",   "by",       "in",      "from",     "for",       "with",        "and",        "to",
    "on",      "named",    "called",  "inside",   "contain",   "contains",    "containing", "using",
    "show",    "list",     "find",    "tell",     "give",      "make",        "create",     "open",
    "delete",  "remove",   "copy",    "move",     "rename",    "only",        "just",       "also",
    "now",     "again",    "instead", "computer", "machine",   "running",     "installed",  "size"};

std::string normalize_word(const std::string& w) {
    std::string s = to_lower(w);
    while (!s.empty() && std::ispunct(static_cast<unsigned char>(s.back())) && s.back() != '\'') s.pop_back();
    while (!s.empty() && (s.front() == '"' || s.front() == '(')) s.erase(0, 1);
    return s;
}

bool looks_like_cmd_switch(const std::string& w) {
    // cmd.exe style: /w /s /b /ad /q /p ... (PowerShell would treat these as paths)
    if (w.size() < 2 || w.size() > 4 || w[0] != '/') return false;
    for (size_t i = 1; i < w.size(); ++i)
        if (!std::isalpha(static_cast<unsigned char>(w[i])) && w[i] != '-' && w[i] != ':') return false;
    return true;
}

}  // namespace

int english_score(const std::string& text, int* word_count) {
    int score = 0, words = 0;
    for (const auto& raw : split_words(text)) {
        std::string w = normalize_word(raw);
        if (w.empty()) continue;
        ++words;
        if (kStrong.count(w)) score += 2;
        else if (kWeak.count(w)) score += 1;
    }
    if (word_count) *word_count = words;
    return score;
}

RouteDecision decide_route(const std::string& input, const json& analysis) {
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
        d.reason = "forced: ask Claude";
        return d;
    }

    const json empty = json::array();
    const json& errors = analysis.contains("parseErrors") && analysis["parseErrors"].is_array()
                             ? analysis["parseErrors"] : empty;
    const json& commands = analysis.contains("commands") && analysis["commands"].is_array()
                               ? analysis["commands"] : empty;

    int words = 0;
    const int raw_score = english_score(line, &words);

    // A question.
    if (line.size() > 1 && line.back() == '?' && words >= 2) {
        d.to_ai = true;
        d.reason = "it reads as a question";
        return d;
    }

    // Parse errors. "Incomplete" means PowerShell wants more lines (an open
    // brace or quote) - real multi-line input, unless it's English with an
    // apostrophe ("what's in here").
    if (!errors.empty()) {
        bool all_incomplete = true;
        for (const auto& e : errors)
            if (!jbool(e, "incomplete", false)) all_incomplete = false;
        if (!all_incomplete) {
            d.to_ai = true;
            d.reason = "it is not valid PowerShell: " + jstr(errors[0], "message", "parse error");
            return d;
        }
        if (raw_score >= 2 && words >= 2) {
            d.to_ai = true;
            d.reason = "it is English with an unclosed quote";
            return d;
        }
        d.reason = "incomplete input; PowerShell continues on the next line";
        return d;
    }

    if (commands.empty()) {
        d.reason = "a PowerShell expression";
        return d;
    }

    // Any command PowerShell can't find: this is where "run it and see if it
    // errors" would fail with CommandNotFound, so nothing would have run anyway.
    for (const auto& c : commands) {
        const std::string kind = jstr(c, "kind");
        if (kind == "NotFound") {
            d.to_ai = true;
            d.reason = "'" + jstr(c, "name") + "' is not a command PowerShell knows";
            return d;
        }
    }

    // Parameters PowerShell would reject: bash habits on aliases ("ls -la",
    // "rm -rf x"), typos, a parameter missing its value. Running it would
    // only produce a binding error before anything happened.
    for (const auto& c : commands) {
        if (c.contains("badParams") && c["badParams"].is_array() && !c["badParams"].empty()) {
            std::string bad;
            for (const auto& p : c["badParams"])
                if (p.is_string()) bad += (bad.empty() ? "" : " ") + p.get<std::string>();
            d.to_ai = true;
            d.reason = "PowerShell would reject " + bad + " on '" + jstr(c, "name") + "' (" +
                       jstr(c, "resolved", jstr(c, "name")) + ")";
            return d;
        }
    }

    // Every command exists. It can still be English that happens to start
    // with an alias ("sort these by size", "where is python", "start the
    // server"), or cmd.exe syntax PowerShell would misread ("dir /w").
    int score = 0, bare_words = 0, params = 0;
    bool codey = false;
    for (const auto& c : commands) {
        params += jint(c, "params", 0);
        if (jint(c, "other", 0) > 0) codey = true;
        const std::string kind = jstr(c, "kind");
        const std::string resolved_kind = jstr(c, "resolvedKind");
        if (c.contains("barewords") && c["barewords"].is_array()) {
            for (const auto& w : c["barewords"]) {
                if (!w.is_string()) continue;
                const std::string word = w.get<std::string>();
                ++bare_words;
                std::string nw = normalize_word(word);
                if (kStrong.count(nw)) score += 2;
                else if (kWeak.count(nw)) score += 1;
                if (kind == "Alias" && resolved_kind == "Cmdlet" && looks_like_cmd_switch(word)) {
                    d.to_ai = true;
                    d.reason = "'" + jstr(c, "name") + " " + word +
                               "' is cmd.exe syntax; in PowerShell '" + jstr(c, "name") + "' is " +
                               jstr(c, "resolved");
                    return d;
                }
            }
        }
    }
    if (params > 0 || commands.size() > 1 || jint(analysis, "redirections", 0) > 0 ||
        jint(analysis, "assignments", 0) > 0)
        codey = true;

    if ((!codey && score >= 2 && bare_words >= 2) || (codey && score >= 5)) {
        d.to_ai = true;
        const auto& first = commands[0];
        d.reason = "it reads like English, though it also parses as PowerShell ('" +
                   jstr(first, "name") + "' is " +
                   (jstr(first, "resolved").empty() ? jstr(first, "kind")
                                                                   : jstr(first, "resolved")) +
                   ")";
        return d;
    }

    d.reason = "valid PowerShell";
    return d;
}

}  // namespace st
