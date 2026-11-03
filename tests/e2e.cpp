// End-to-end test: runs st.exe inside a pseudo console, types at it like a
// person would, and checks what appears on screen.
//
//   st_e2e.exe <path to st.exe> [transcript file] [--offline]
//
// Without --offline it needs ANTHROPIC_API_KEY and makes a few real Claude calls.

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "util.h"

using namespace std::chrono;

namespace {

struct Session {
    HPCON hpc = nullptr;
    HANDLE in_write = nullptr, out_read = nullptr;
    PROCESS_INFORMATION pi{};
    std::mutex mu;
    std::string raw;
    std::thread reader;

    bool start(const std::wstring& cmd) {
        HANDLE in_read, out_write;
        CreatePipe(&in_read, &in_write, nullptr, 0);
        CreatePipe(&out_read, &out_write, nullptr, 0);
        if (FAILED(CreatePseudoConsole({120, 40}, in_read, out_write, 0, &hpc))) return false;
        CloseHandle(in_read);
        CloseHandle(out_write);
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::vector<char> buf(size);
        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof si;
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buf.data());
        InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size);
        UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hpc, sizeof hpc, nullptr,
                                  nullptr);
        std::wstring c = cmd;
        if (!CreateProcessW(nullptr, c.data(), nullptr, nullptr, FALSE, EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                            &si.StartupInfo, &pi))
            return false;
        reader = std::thread([this] {
            char b[8192];
            DWORD n;
            while (ReadFile(out_read, b, sizeof b, &n, nullptr) && n) {
                std::lock_guard lock(mu);
                raw.append(b, n);
            }
        });
        return true;
    }

    std::string text() {
        std::lock_guard lock(mu);
        return st::strip_vt(raw);
    }

    void type(const std::string& s) {
        DWORD w;
        WriteFile(in_write, s.data(), static_cast<DWORD>(s.size()), &w, nullptr);
    }

    // Waits until `needle` appears at or after offset `from` of the screen text.
    bool wait_for(const std::string& needle, size_t from, int seconds, size_t* found_at = nullptr) {
        auto deadline = steady_clock::now() + std::chrono::seconds(seconds);
        while (steady_clock::now() < deadline) {
            std::string t = text();
            size_t p = t.find(needle, std::min(from, t.size()));
            if (p != std::string::npos) {
                if (found_at) *found_at = p;
                return true;
            }
            if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) return false;
            Sleep(100);
        }
        return false;
    }

    int finish(int seconds) {
        DWORD r = WaitForSingleObject(pi.hProcess, seconds * 1000);
        DWORD code = 999;
        if (r == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
        else TerminateProcess(pi.hProcess, 998);
        ClosePseudoConsole(hpc);
        reader.join();
        return static_cast<int>(code);
    }
};

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    std::fflush(stdout);
    if (!ok) ++failures;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: st_e2e <st.exe> [transcript] [--offline] [--direct]\n");
        return 2;
    }
    std::wstring exe = argv[1];
    std::wstring transcript = argc >= 3 && argv[2][0] != L'-' ? argv[2] : L"e2e-transcript.txt";
    bool offline = false, direct = false;
    std::wstring shell;
    for (int i = 2; i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--offline") offline = true;
        if (std::wstring(argv[i]) == L"--direct") direct = true;
        if (std::wstring(argv[i]) == L"--shell" && i + 1 < argc) shell = argv[++i];
    }
    const bool posix = !shell.empty() && shell.rfind(L"pwsh", 0) != 0 && shell.rfind(L"powershell", 0) != 0;

    // The same checks in each shell's own syntax.
    struct Script {
        const char *prompt, *plain, *multi1, *multi2, *setvar, *sleep, *after_cancel, *after_ctrl_c;
    };
    const Script ps = {"PS ", "echo ('st' + '-plain-ok')\r", "if ($true) {\r", "'st-multi' + '-ok' }\r",
                       "$stvar = 41 + 1; \"var=$stvar\"\r", "Start-Sleep -Seconds 60; 'st-sleep-' + 'finished'\r",
                       "echo ('st' + '-after-cancel')\r", "echo ('st' + '-after-ctrl-c')\r"};
    const Script sh = {"$", "echo \"st\"\"-plain-ok\"\r", "if true; then\r", "echo \"st-multi\"\"-ok\"; fi\r",
                       "stvar=$((41 + 1)); echo \"var=$stvar\"\r", "sleep 60; echo st-sleep-finished\r",
                       "echo \"st\"\"-after-cancel\"\r", "echo \"st\"\"-after-ctrl-c\"\r"};
    const Script& sc = posix ? sh : ps;

    Session s;
    std::wstring cmd = L"\"" + exe + L"\"" + (direct ? L" --direct" : L"") + (shell.empty() ? L"" : L" --shell " + shell);
    if (!s.start(cmd)) {
        std::fprintf(stderr, "couldn't start %s\n", st::narrow(exe).c_str());
        return 2;
    }

    size_t at = 0;
    check(s.wait_for("SuperTerminal", 0, 30, &at), "banner appears");
    check(s.wait_for(sc.prompt, at, 40, &at), "shell prompt appears");
    Sleep(500);

    // 1. Ordinary commands run untouched.
    s.type(sc.plain);
    check(s.wait_for("st-plain-ok\n", at, 20, &at), "plain command runs");

    // 2. Multi-line input still works (incomplete input continues).
    s.type(sc.multi1);
    Sleep(600);
    s.type(sc.multi2);
    check(s.wait_for("st-multi-ok\n", at, 20, &at), "multi-line input continues and runs");

    // 3. Setting a variable and reading it back: the session is one live session.
    s.type(sc.setvar);
    check(s.wait_for("var=42", at, 20, &at), "session state persists");

    if (!offline) {
        // 4. Plain English, read-only: Claude writes a command and it runs straight away.
        s.type("show me the files in this folder whose names start with CMakeL\r");
        if (!posix) check(s.wait_for("Get-ChildItem", at, 60, &at), "the English request became a command on the prompt");
        check(s.wait_for("CMakeLists.txt", at, 60, &at), "the command ran by itself and listed CMakeLists.txt");
        check(s.wait_for(sc.prompt, at, 30, &at), "back at a prompt");
        Sleep(500);

        // 5. A follow-up that relies on the previous request.
        s.type("now just the count of them\r");
        check(s.wait_for("\n1\n", at, 60, &at), "follow-up request is understood (count = 1)");
        check(s.wait_for(sc.prompt, at, 30, &at), "back at a prompt after the follow-up");
        Sleep(500);

        // 6. Something that changes files waits for Enter; Esc cancels it.
        s.type(posix ? "touch zz-st-e2e.tmp\r" : "$null = New-Item zz-st-e2e.tmp -Force\r");
        Sleep(800);
        s.type("delete the file called zz-st-e2e.tmp\r");
        check(s.wait_for("Enter to run", at, 60, &at), "a deleting command waits for confirmation");
        Sleep(800);
        s.type("\x1b");
        Sleep(1200);
        s.type(sc.after_cancel);
        check(s.wait_for("st-after-cancel\n", at, 20, &at), "Esc cancelled it and the prompt still works");
        s.type(posix ? "test -f zz-st-e2e.tmp && echo \"st-still\"\"-there\"; rm -f zz-st-e2e.tmp\r"
                     : "if (Test-Path zz-st-e2e.tmp) { 'st-still' + '-there' }; Remove-Item zz-st-e2e.tmp\r");
        check(s.wait_for("st-still-there\n", at, 20, &at), "the cancelled delete never ran (file still there)");

        // 7. cmd.exe habits are translated.
        s.type("dir /w\r");
        check(s.wait_for("third_party", at, 60, &at), "'dir /w' is translated and lists the folder");
        check(s.wait_for(sc.prompt, at, 30, &at), "back at a prompt after 'dir /w'");
        Sleep(500);
    }

    // Ctrl+C while a command runs stops that command, not SuperTerminal.
    s.type(sc.sleep);
    Sleep(2500);
    s.type("\x03");
    Sleep(1000);
    s.type(sc.after_ctrl_c);
    check(s.wait_for("st-after-ctrl-c\n", at, 15, &at), "Ctrl+C stops a running command and the session carries on");

    // Ctrl+C at the prompt exits.
    Sleep(500);
    s.type("\x03");
    int code = s.finish(20);
    check(code == 0, "Ctrl+C at the prompt exits SuperTerminal");

    // Ctrl+X at the prompt exits too (a second, short session).
    {
        Session x;
        if (x.start(cmd)) {
            size_t xa = 0;
            x.wait_for(sc.prompt, 0, 40, &xa);
            Sleep(800);
            x.type("\x18");
            check(x.finish(20) == 0, "Ctrl+X at the prompt exits SuperTerminal");
        } else {
            check(false, "Ctrl+X at the prompt exits SuperTerminal (couldn't start)");
        }
    }

    // Nothing on screen gives away what is behind the translation.
    {
        std::string screen = st::to_lower(s.text());
        // This repo has a CLAUDE.md, which `dir /w` lists; that's file output, not a giveaway.
        for (size_t p; (p = screen.find("claude.md")) != std::string::npos;) screen.erase(p, 9);
        for (size_t p; (p = screen.find("anthropic_api_key")) != std::string::npos;) screen.erase(p, 17);
        bool clean = true;
        for (const char* word : {"claude", "opus", "sonnet", "haiku", "anthropic", "thinking", " llm", "\xC2\xBB"})
            if (screen.find(word) != std::string::npos) {
                std::printf("      found \"%s\" on screen\n", word);
                clean = false;
            }
        check(clean, "no mention of the model or AI on screen");
    }

    std::ofstream(transcript, std::ios::binary) << s.text();
    std::printf("\n%d failure(s). Transcript: %s\n", failures, st::narrow(transcript).c_str());
    return failures;
}
