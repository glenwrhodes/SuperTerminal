#include "context.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>

#include "util.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace st {

#ifdef _WIN32
static std::wstring reg_string(const wchar_t* name) {
    wchar_t buf[256];
    DWORD size = sizeof buf;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", name, RRF_RT_REG_SZ,
                     nullptr, buf, &size) == ERROR_SUCCESS)
        return buf;
    return {};
}

std::string os_description() {
    std::string product = narrow(reg_string(L"ProductName"));
    std::string display = narrow(reg_string(L"DisplayVersion"));
    std::string build = narrow(reg_string(L"CurrentBuild"));
    // ProductName still says "Windows 10" on Windows 11.
    if (!build.empty() && std::atoi(build.c_str()) >= 22000) {
        size_t p = product.find("Windows 10");
        if (p != std::string::npos) product.replace(p, 10, "Windows 11");
    }
    if (product.empty()) product = "Windows";
    std::string out = product;
    if (!display.empty()) out += " " + display;
    if (!build.empty()) out += " (build " + build + ")";
    return out;
}

std::string user_home() { return read_env("USERPROFILE"); }
#else
std::string os_description() {
#ifdef __APPLE__
    std::string v = run_capture("sw_vers -productVersion 2>/dev/null");
    return "macOS" + (v.empty() ? std::string() : " " + v);
#else
    std::ifstream f("/etc/os-release");
    std::string line, pretty;
    while (std::getline(f, line))
        if (starts_with(line, "PRETTY_NAME=")) {
            pretty = line.substr(12);
            if (pretty.size() >= 2 && pretty.front() == '"') pretty = pretty.substr(1, pretty.size() - 2);
        }
    std::string kernel = run_capture("uname -r 2>/dev/null");
    std::string out = pretty.empty() ? "Linux" : pretty;
    if (kernel.find("microsoft") != std::string::npos || kernel.find("WSL") != std::string::npos)
        out += " (WSL on Windows)";
    return out;
#endif
}

std::string user_home() { return read_env("HOME"); }
#endif

std::string folder_listing(const std::string& folder_utf8, size_t max_entries) {
    struct Entry {
        std::string name;
        bool dir;
        unsigned long long size;
    };
    std::vector<Entry> entries;
    std::error_code ec;
    fs::directory_iterator it(path_from_utf8(folder_utf8), fs::directory_options::skip_permission_denied, ec);
    if (ec) return "  (could not list this folder: " + ec.message() + ")\n";
    size_t total = 0;
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        ++total;
        if (entries.size() >= 400) continue;  // enough to sort and sample from
        Entry e;
        e.name = path_to_utf8(it->path().filename());
        e.dir = it->is_directory(ec);
        e.size = e.dir ? 0 : it->file_size(ec);
        if (ec) e.size = 0, ec.clear();
        entries.push_back(std::move(e));
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.dir != b.dir) return a.dir;
        return to_lower(a.name) < to_lower(b.name);
    });
    if (total == 0) return "  (empty folder)\n";
    std::string out;
    size_t shown = 0;
    for (const auto& e : entries) {
        if (shown >= max_entries) break;
        out += e.dir ? "  [dir]  " + e.name + "/\n" : "  " + human_size(e.size) + "  " + e.name + "\n";
        ++shown;
    }
    if (total > shown) out += "  ... and " + std::to_string(total - shown) + " more entries\n";
    return out;
}

}  // namespace st
