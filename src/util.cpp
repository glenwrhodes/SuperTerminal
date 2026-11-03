#include "util.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <random>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <unistd.h>
#include <limits.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace fs = std::filesystem;

namespace st {

std::string trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

std::vector<std::string> split_words(std::string_view s) {
    std::vector<std::string> words;
    std::string cur;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) words.push_back(std::move(cur)), cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) words.push_back(std::move(cur));
    return words;
}

std::vector<std::string> split_lines(std::string_view s) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < s.size()) {
        size_t nl = s.find('\n', start);
        if (nl == std::string_view::npos) nl = s.size();
        std::string line(s.substr(start, nl - start));
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        start = nl + 1;
    }
    return lines;
}

#ifdef _WIN32

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string narrow(std::wstring_view s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

static std::string read_registry_env(HKEY root, const wchar_t* subkey, const wchar_t* name) {
    DWORD size = 0;
    if (RegGetValueW(root, subkey, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, nullptr,
                     &size) != ERROR_SUCCESS || size == 0)
        return {};
    std::wstring buf(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, subkey, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, buf.data(),
                     &size) != ERROR_SUCCESS)
        return {};
    buf.resize(wcsnlen(buf.c_str(), buf.size()));
    return narrow(buf);
}

std::string read_env(const char* name_utf8) {
    std::wstring name = widen(name_utf8);
    DWORD n = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
    if (n > 1) {
        std::wstring buf(n, L'\0');
        GetEnvironmentVariableW(name.c_str(), buf.data(), n);
        buf.resize(n - 1);
        return trim(narrow(buf));
    }
    std::string v = read_registry_env(HKEY_CURRENT_USER, L"Environment", name.c_str());
    if (v.empty())
        v = read_registry_env(HKEY_LOCAL_MACHINE,
                              L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment", name.c_str());
    return trim(v);
}

void set_env(const char* name, const std::string& value) {
    SetEnvironmentVariableW(widen(name).c_str(), widen(value).c_str());
}

std::string win32_error(unsigned long code) {
    wchar_t* msg = nullptr;
    HMODULE winhttp = GetModuleHandleW(L"winhttp.dll");
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    if (winhttp) flags |= FORMAT_MESSAGE_FROM_HMODULE;
    FormatMessageW(flags, winhttp, code, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::string out = msg ? trim(narrow(msg)) : std::string();
    if (msg) LocalFree(msg);
    if (out.empty()) out = "error " + std::to_string(code);
    return out;
}

fs::path path_from_utf8(const std::string& s) { return fs::path(widen(s)); }
std::string path_to_utf8(const fs::path& p) { return narrow(p.wstring()); }

static fs::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR base = nullptr;
    fs::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &base))) {
        dir = fs::path(base) / L"SuperTerminal";
        CoTaskMemFree(base);
    } else {
        dir = fs::temp_directory_path() / L"SuperTerminal";
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

fs::path config_dir() { return known_folder(FOLDERID_RoamingAppData); }
fs::path cache_dir() { return known_folder(FOLDERID_LocalAppData); }
fs::path temp_dir() { return fs::temp_directory_path(); }

std::string self_exe_path() {
    wchar_t buf[32768];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return narrow(std::wstring_view(buf, n));
}

std::string run_capture(const std::string&) { return {}; }

#else  // ------------------------------------------------------------- Unix

std::string read_env(const char* name) {
    const char* v = std::getenv(name);
    return v ? trim(v) : std::string();
}

void set_env(const char* name, const std::string& value) { setenv(name, value.c_str(), 1); }

fs::path path_from_utf8(const std::string& s) { return fs::path(s); }
std::string path_to_utf8(const fs::path& p) { return p.string(); }

static fs::path xdg_dir(const char* var, const char* fallback) {
    std::string base = read_env(var);
    fs::path dir = base.empty() ? fs::path(read_env("HOME")) / fallback : fs::path(base);
    dir /= "superterminal";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

fs::path config_dir() { return xdg_dir("XDG_CONFIG_HOME", ".config"); }
fs::path cache_dir() { return xdg_dir("XDG_CACHE_HOME", ".cache"); }

fs::path temp_dir() {
    // Must match .NET's Path.GetTempPath() so PowerShell's NamedPipeClientStream finds our socket.
    std::string t = read_env("TMPDIR");
    return t.empty() ? fs::path("/tmp") : fs::path(t);
}

std::string self_exe_path() {
#ifdef __APPLE__
    char buf[PATH_MAX];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0) {
        std::error_code ec;
        auto p = fs::canonical(buf, ec);
        return ec ? std::string(buf) : p.string();
    }
    return "st";
#else
    std::error_code ec;
    auto p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? std::string("st") : p.string();
#endif
}

std::string run_capture(const std::string& command) {
    std::string out;
    FILE* f = popen(command.c_str(), "r");
    if (!f) return out;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    pclose(f);
    return trim(out);
}

#endif

std::string strip_vt(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    const size_t n = in.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c == 0x1b) {
            if (i + 1 >= n) break;
            const char k = in[i + 1];
            if (k == '[') {  // CSI: params, intermediates, final byte
                size_t j = i + 2;
                std::string params;
                while (j < n && in[j] >= 0x30 && in[j] <= 0x3f) params += in[j++];
                while (j < n && in[j] >= 0x20 && in[j] <= 0x2f) ++j;
                if (j >= n) break;
                const char fin = in[j];
                if (fin == 'C') {  // ConPTY writes runs of spaces as "cursor forward"
                    int count = params.empty() ? 1 : std::atoi(params.c_str());
                    out.append(static_cast<size_t>(std::clamp(count, 1, 300)), ' ');
                } else if (fin == 'H' || fin == 'f' || fin == 'E') {
                    if (!out.empty() && out.back() != '\n') out += '\n';
                }
                i = j + 1;
                continue;
            }
            if (k == ']' || k == 'P' || k == '_' || k == '^') {  // OSC / DCS / APC / PM: up to BEL or ST
                size_t j = i + 2;
                while (j < n && in[j] != '\a' && !(in[j] == 0x1b && j + 1 < n && in[j + 1] == '\\')) ++j;
                i = (j < n && in[j] == '\a') ? j + 1 : j + 2;
                continue;
            }
            if (k == '(' || k == ')' || k == '*' || k == '+' || k == '#') {
                i += 3;
                continue;
            }
            i += 2;
            continue;
        }
        if (c == '\r') {
            ++i;
            continue;
        }
        if (c == '\b') {
            if (!out.empty() && out.back() != '\n') out.pop_back();
            ++i;
            continue;
        }
        if (c < 0x20 && c != '\n' && c != '\t') {
            ++i;
            continue;
        }
        out += static_cast<char>(c);
        ++i;
    }

    // Trim trailing spaces on each line and squeeze runs of blank lines.
    std::string cleaned;
    cleaned.reserve(out.size());
    size_t blank_run = 0;
    for (auto& line : split_lines(out)) {
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (line.empty()) {
            if (++blank_run <= 1 && !cleaned.empty()) cleaned += '\n';
        } else {
            blank_run = 0;
            cleaned += line;
            cleaned += '\n';
        }
    }
    return cleaned;
}

std::string tail(std::string_view s, size_t max_bytes) {
    if (s.size() <= max_bytes) return std::string(s);
    size_t cut = s.size() - max_bytes;
    size_t nl = s.find('\n', cut);
    if (nl != std::string_view::npos && nl + 1 < s.size() && nl - cut < 200) cut = nl + 1;
    while (cut < s.size() && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) ++cut;
    return std::string(s.substr(cut));
}

std::string human_size(unsigned long long bytes) {
    char buf[32];
    if (bytes < 1024) {
        std::snprintf(buf, sizeof buf, "%llu B", bytes);
    } else if (bytes < 1024ull * 1024) {
        std::snprintf(buf, sizeof buf, "%.1f KB", bytes / 1024.0);
    } else if (bytes < 1024ull * 1024 * 1024) {
        std::snprintf(buf, sizeof buf, "%.1f MB", bytes / (1024.0 * 1024));
    } else {
        std::snprintf(buf, sizeof buf, "%.2f GB", bytes / (1024.0 * 1024 * 1024));
    }
    return buf;
}

}  // namespace st
