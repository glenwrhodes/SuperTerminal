#include "scripts.h"

#include <fstream>

#include "util.h"

#ifdef _WIN32
#include <process.h>
#define st_getpid _getpid
#else
#include <unistd.h>
#define st_getpid getpid
#endif

namespace fs = std::filesystem;

namespace st {

namespace {
unsigned long long fnv1a(std::string_view s) {
    unsigned long long h = 1469598103934665603ull;
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}

// Writes content to path unless an identical file is already there.
bool write_if_changed(const fs::path& path, const std::string& content, std::string* error) {
    std::error_code ec;
    if (fs::exists(path, ec) && fs::file_size(path, ec) == content.size()) return true;
    fs::path tmp = path;
    tmp += "." + std::to_string(st_getpid()) + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!f) {
            if (error) *error = "couldn't write " + path_to_utf8(tmp);
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        if (!fs::exists(path, ec)) {
            if (error) *error = "couldn't write " + path_to_utf8(path);
            return false;
        }
    }
    return true;
}
}  // namespace

fs::path install_script(const char* name, std::string* error) {
    std::string dev = read_env("SUPERTERMINAL_SCRIPT_DIR");
    if (!dev.empty()) return path_from_utf8(dev) / name;

    std::string content(embedded_file(name));
    if (content.empty()) {
        if (error) *error = std::string("the script ") + name + " is missing from this build";
        return {};
    }
    std::string ext = fs::path(name).extension().string();
    if (ext == ".bash" || ext == ".zsh") {  // must have Unix line endings, whatever git did on checkout
        std::string lf;
        lf.reserve(content.size());
        for (char c : content)
            if (c != '\r') lf += c;
        content.swap(lf);
    }
    char hash[32];
    std::snprintf(hash, sizeof hash, "%016llx", fnv1a(content));
    fs::path path = cache_dir() / (fs::path(name).stem().string() + "-" + hash + ext);
    if (!write_if_changed(path, content, error)) return {};
    return path;
}

fs::path prepare_zdotdir(const fs::path& hook, std::string* error) {
    fs::path dir = cache_dir() / "zdotdir";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string user = "${SUPERTERMINAL_USER_ZDOTDIR:-$HOME}";
    // Each wrapper runs the user's own file; .zshrc then hands ZDOTDIR back and loads our hook.
    // Keep ZDOTDIR pointing here while the user's .zshenv runs; if it sets its
    // own ZDOTDIR (common: ~/.config/zsh), remember that as the user's.
    const std::string zshenv =
        "__st_zdot=$ZDOTDIR\n"
        "ZDOTDIR=\"" + user + "\"\n"
        "[[ -f \"$ZDOTDIR/.zshenv\" ]] && source \"$ZDOTDIR/.zshenv\"\n"
        "export SUPERTERMINAL_USER_ZDOTDIR=\"$ZDOTDIR\"\n"
        "ZDOTDIR=$__st_zdot\n"
        "unset __st_zdot\n";
    const std::string zprofile = "[[ -f \"" + user + "/.zprofile\" ]] && source \"" + user + "/.zprofile\"\n";
    const std::string zshrc = "ZDOTDIR=\"" + user + "\"\n"
                              "[[ -f \"$ZDOTDIR/.zshrc\" ]] && source \"$ZDOTDIR/.zshrc\"\n"
                              "source \"" + path_to_utf8(hook) + "\"\n";
    if (!write_if_changed(dir / ".zshenv", zshenv, error)) return {};
    if (!write_if_changed(dir / ".zprofile", zprofile, error)) return {};
    // .zshrc changes with the hook path, so always rewrite it.
    std::ofstream f(dir / ".zshrc", std::ios::binary | std::ios::trunc);
    f << zshrc;
    if (!f) {
        if (error) *error = "couldn't write " + path_to_utf8(dir / ".zshrc");
        return {};
    }
    return dir;
}

}  // namespace st
