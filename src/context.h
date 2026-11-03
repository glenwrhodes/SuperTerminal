#pragma once
#include <string>

namespace st {

// "Windows 11 Pro 25H2 (build 26200)", "macOS 15.1", "Ubuntu 24.04.2 LTS"
std::string os_description();

// A compact listing of a folder: folders first, then files with sizes, at
// most max_entries lines plus a count of what was left out.
std::string folder_listing(const std::string& folder_utf8, size_t max_entries = 80);

std::string user_home();

}  // namespace st
