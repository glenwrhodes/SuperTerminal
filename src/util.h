#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace st {

std::string trim(std::string_view s);
std::string to_lower(std::string_view s);
bool starts_with(std::string_view s, std::string_view prefix);
std::vector<std::string> split_words(std::string_view s);
std::vector<std::string> split_lines(std::string_view s);

// An environment variable. On Windows this falls back to the user's and the
// machine's registry environment, so a variable set with setx / System
// Properties is found even in a console opened before it was set.
std::string read_env(const char* name);
void set_env(const char* name, const std::string& value);

// UTF-8 <-> filesystem paths.
std::filesystem::path path_from_utf8(const std::string& s);
std::string path_to_utf8(const std::filesystem::path& p);

// Settings: %APPDATA%\SuperTerminal, or $XDG_CONFIG_HOME/superterminal (~/.config/superterminal).
std::filesystem::path config_dir();
// Generated files: %LOCALAPPDATA%\SuperTerminal, or $XDG_CACHE_HOME/superterminal (~/.cache/superterminal).
std::filesystem::path cache_dir();
// Where sockets live on Unix ($TMPDIR or /tmp) - the same place .NET puts named pipes.
std::filesystem::path temp_dir();

// Full path of the running st binary.
std::string self_exe_path();

// Turns terminal output (UTF-8 with VT escape sequences) into plain text.
std::string strip_vt(std::string_view raw);

// Keeps the last max_bytes of s, cut at a line boundary when possible.
std::string tail(std::string_view s, size_t max_bytes);

std::string human_size(unsigned long long bytes);

// Output of a short command (Unix only; empty on Windows or on failure).
std::string run_capture(const std::string& command);

#ifdef _WIN32
std::wstring widen(std::string_view s);
std::string narrow(std::wstring_view s);
std::string win32_error(unsigned long code);
#endif

}  // namespace st
