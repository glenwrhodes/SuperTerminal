#include "pty_host.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "output_log.h"
#include "util.h"

namespace st {

namespace {

COORD window_size(HANDLE out) {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(out, &info)) return {120, 30};
    SHORT w = static_cast<SHORT>(info.srWindow.Right - info.srWindow.Left + 1);
    SHORT h = static_cast<SHORT>(info.srWindow.Bottom - info.srWindow.Top + 1);
    if (w <= 0 || h <= 0) return {120, 30};
    return {w, h};
}

void append_utf8(std::string& out, wchar_t c, wchar_t& pending_high) {
    if (c >= 0xD800 && c <= 0xDBFF) {
        pending_high = c;
        return;
    }
    std::wstring w;
    if (c >= 0xDC00 && c <= 0xDFFF && pending_high) {
        w += pending_high;
        pending_high = 0;
    }
    w += c;
    out += narrow(w);
}

// win32-input-mode: ESC [ Vk ; Sc ; Uc ; Kd ; Cs ; Rc _
// Carries the complete key event, so PSReadLine sees exactly the keys the
// user pressed (Ctrl+Space, Shift+Enter, Alt combinations, key-ups...).
void encode_win32(std::string& out, const KEY_EVENT_RECORD& k) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "\x1b[%u;%u;%u;%u;%lu;%u_", static_cast<unsigned>(k.wVirtualKeyCode),
                  static_cast<unsigned>(k.wVirtualScanCode), static_cast<unsigned>(k.uChar.UnicodeChar),
                  k.bKeyDown ? 1u : 0u, static_cast<unsigned long>(k.dwControlKeyState),
                  static_cast<unsigned>(k.wRepeatCount ? k.wRepeatCount : 1));
    out += buf;
}

// Ctrl+C / Ctrl+Break belong to the shell, not to us. A handler function is
// used rather than SetConsoleCtrlHandler(nullptr, TRUE), whose "ignore Ctrl+C"
// flag is inherited by child processes: PowerShell would then ignore it too.
BOOL WINAPI swallow_ctrl_c(DWORD type) { return type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT; }

}  // namespace

bool has_console() {
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) &&
           GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode);
}

int run_direct(const HostOptions& opt) {
    SetConsoleCtrlHandler(swallow_ctrl_c, TRUE);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = opt.command_line;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        std::fprintf(stderr, "SuperTerminal: couldn't start the shell: %s\n", win32_error(GetLastError()).c_str());
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

int run_in_pseudoconsole(const HostOptions& opt) {
    HANDLE con_in = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE con_out = GetStdHandle(STD_OUTPUT_HANDLE);

    HANDLE in_read = nullptr, in_write = nullptr, out_read = nullptr, out_write = nullptr;
    if (!CreatePipe(&in_read, &in_write, nullptr, 0) || !CreatePipe(&out_read, &out_write, nullptr, 0)) return -1;

    COORD size = window_size(con_out);
    HPCON hpc = nullptr;
    if (FAILED(CreatePseudoConsole(size, in_read, out_write, 0, &hpc))) {
        CloseHandle(in_read), CloseHandle(in_write), CloseHandle(out_read), CloseHandle(out_write);
        return -1;
    }
    // The pseudo console holds its own copies of these.
    CloseHandle(in_read);
    CloseHandle(out_write);

    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<char> attr_buf(attr_size);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    // Null std handles: the shell must use the pseudo console, never handles we were given.
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_size) ||
        !UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hpc, sizeof hpc,
                                   nullptr, nullptr)) {
        ClosePseudoConsole(hpc);
        return -1;
    }

    PROCESS_INFORMATION pi{};
    std::wstring cmd = opt.command_line;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                        &si.StartupInfo, &pi)) {
        DWORD err = GetLastError();
        DeleteProcThreadAttributeList(si.lpAttributeList);
        ClosePseudoConsole(hpc);
        std::fprintf(stderr, "SuperTerminal: couldn't start the shell: %s\n", win32_error(err).c_str());
        return 1;
    }
    DeleteProcThreadAttributeList(si.lpAttributeList);

    // If SuperTerminal is closed or killed, take the shell with it.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
        AssignProcessToJobObject(job, pi.hProcess);
    }

    // Our console: raw keys in, VT out, UTF-8.
    DWORD saved_in_mode = 0, saved_out_mode = 0;
    GetConsoleMode(con_in, &saved_in_mode);
    GetConsoleMode(con_out, &saved_out_mode);
    const UINT saved_out_cp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleMode(con_out, ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                                DISABLE_NEWLINE_AUTO_RETURN);
    DWORD in_mode = ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS | (saved_in_mode & ENABLE_QUICK_EDIT_MODE);
    if (!opt.win32_input) in_mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    SetConsoleMode(con_in, in_mode);
    SetConsoleCtrlHandler(swallow_ctrl_c, TRUE);

    std::atomic<bool> done{false};

    std::thread output([&] {
        std::vector<char> buf(32 * 1024);
        DWORD n = 0;
        while (ReadFile(out_read, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr) && n > 0) {
            DWORD written = 0;
            WriteFile(con_out, buf.data(), n, &written, nullptr);
            if (opt.log) opt.log->append(std::string_view(buf.data(), n));
        }
    });

    std::thread input([&] {
        std::vector<INPUT_RECORD> recs(128);
        wchar_t pending_high = 0;
        while (!done) {
            DWORD n = 0;
            if (!ReadConsoleInputW(con_in, recs.data(), static_cast<DWORD>(recs.size()), &n)) break;
            std::string bytes;
            for (DWORD i = 0; i < n; ++i) {
                const INPUT_RECORD& r = recs[i];
                if (r.EventType == KEY_EVENT) {
                    if (opt.win32_input) {
                        encode_win32(bytes, r.Event.KeyEvent);
                    } else if (r.Event.KeyEvent.bKeyDown && r.Event.KeyEvent.uChar.UnicodeChar) {
                        for (WORD k = 0; k < (r.Event.KeyEvent.wRepeatCount ? r.Event.KeyEvent.wRepeatCount : 1); ++k)
                            append_utf8(bytes, r.Event.KeyEvent.uChar.UnicodeChar, pending_high);
                    }
                }
            }
            if (!bytes.empty()) {
                DWORD written = 0;
                if (!WriteFile(in_write, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)) break;
            }
        }
    });

    // Window size: poll rather than trust resize events, which classic
    // console windows with a scrollback buffer don't send for height changes.
    std::thread resizer([&] {
        COORD last = size;
        while (!done) {
            Sleep(150);
            COORD now = window_size(con_out);
            if (now.X != last.X || now.Y != last.Y) {
                ResizePseudoConsole(hpc, now);
                last = now;
            }
        }
    });

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    done = true;

    ClosePseudoConsole(hpc);  // ends the output stream once drained
    output.join();
    resizer.join();
    input.detach();  // blocked reading the console; ends with the process

    SetConsoleMode(con_in, saved_in_mode);
    SetConsoleMode(con_out, saved_out_mode);
    SetConsoleOutputCP(saved_out_cp);
    CloseHandle(out_read);
    CloseHandle(in_write);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);
    return static_cast<int>(code);
}

}  // namespace st
