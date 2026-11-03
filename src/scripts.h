#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace st {

// The shell integration scripts in shell/, compiled in by cmake/embed.cmake.
std::string_view embedded_file(const char* name);

// Writes an embedded script ("superterminal.ps1", ".bash", ".zsh") to
// cache_dir(), named after its content, and returns the path. With
// SUPERTERMINAL_SCRIPT_DIR=<repo>/shell set, uses that folder's file directly
// (handy while working on the scripts).
std::filesystem::path install_script(const char* name, std::string* error);

// zsh has no --rcfile, so st points ZDOTDIR at a folder whose startup files
// load the user's own (from their real ZDOTDIR or $HOME) and then our hook.
std::filesystem::path prepare_zdotdir(const std::filesystem::path& hook, std::string* error);

}  // namespace st
