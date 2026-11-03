#pragma once
#include <string>
#include <vector>

namespace st {

class OutputLog;

struct HostOptions {
#ifdef _WIN32
    std::wstring command_line;      // e.g. "pwsh.exe -NoLogo -NoExit -Command ..."
    bool win32_input = true;        // forward keys in win32-input-mode (full fidelity) instead of plain VT
#else
    std::vector<std::string> argv;  // e.g. {"/bin/bash", "--rcfile", "...", "-i"}
#endif
    OutputLog* log = nullptr;       // receives a copy of everything the shell prints
};

// Runs the shell inside a pseudo terminal (ConPTY on Windows, forkpty on
// Unix) and relays it to this terminal: output passes through (and is copied
// to the log), keys and window resizes pass in. Returns the shell's exit code,
// or -1 if the pseudo terminal couldn't be created (then use run_direct).
int run_in_pseudoconsole(const HostOptions& options);

// Runs the shell directly in this terminal. Interactive fidelity is perfect,
// but SuperTerminal can't see the output.
int run_direct(const HostOptions& options);

// True when stdin and stdout are both a real terminal.
bool has_console();

}  // namespace st
