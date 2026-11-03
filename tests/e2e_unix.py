#!/usr/bin/env python3
"""End-to-end test for macOS / Linux: runs st in a pseudo terminal, types at it
like a person would, and checks the screen. The Windows twin is tests/e2e.cpp.

    python3 tests/e2e_unix.py <path to st> [--shell bash|zsh|pwsh] [--offline]

Runs with a throwaway HOME, so your own shell startup files and settings stay
out of it. Without --offline it makes a few real requests (ANTHROPIC_API_KEY).
"""
import os
import pty
import re
import select
import shutil
import signal
import struct
import sys
import tempfile
import time

try:
    import fcntl
    import termios
except ImportError:  # pragma: no cover
    sys.exit("This test needs a Unix pty (use tests/e2e.cpp on Windows).")

ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]|\x1b\][^\x07\x1b]*(\x07|\x1b\\)|\x1b[()][A-Za-z0-9]|\x1b[=>78]")


class Session:
    def __init__(self, argv, env):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.execvpe(argv[0], argv, env)
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))
        self.raw = b""
        self.code = None

    def pump(self, timeout=0.1):
        r, _, _ = select.select([self.fd], [], [], timeout)
        if r:
            try:
                data = os.read(self.fd, 65536)
            except OSError:
                data = b""
            if not data:
                return False
            self.raw += data
            # Answer cursor-position queries like a real terminal would (.NET / PowerShell
            # on Unix asks for it and waits). A rough position is enough for the test.
            for _ in range(data.count(b"\x1b[6n")):
                os.write(self.fd, b"\x1b[20;1R")
        return True

    def text(self):
        t = ANSI.sub("", self.raw.decode("utf-8", "replace")).replace("\r", "")
        return t

    def type(self, s, pause=0.02):
        for ch in s:  # one key at a time, like a person
            os.write(self.fd, ch.encode())
            self.pump(pause)  # keep answering the shell's terminal queries while typing

    def wait_for(self, needle, start, seconds):
        deadline = time.time() + seconds
        while time.time() < deadline:
            t = self.text()
            p = t.find(needle, min(start, len(t)))
            if p >= 0:
                return p
            if not self.pump(0.1):
                break
        return -1

    def idle(self, seconds):
        """Wait while still answering the shell's terminal queries (a real terminal always does)."""
        deadline = time.time() + seconds
        while time.time() < deadline:
            self.pump(0.05)

    def finish(self, seconds):
        deadline = time.time() + seconds
        while time.time() < deadline:
            pid, status = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                self.code = os.waitstatus_to_exitcode(status)
                return self.code
            self.pump(0.1)
        os.kill(self.pid, signal.SIGKILL)
        os.waitpid(self.pid, 0)
        return None


failures = 0


def check(ok, what):
    global failures
    print(("PASS  " if ok else "FAIL  ") + what, flush=True)
    failures += 0 if ok else 1


def main():
    args = sys.argv[1:]
    exe = os.path.abspath(args[0])
    shell = args[args.index("--shell") + 1] if "--shell" in args else "bash"
    offline = "--offline" in args
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    home = tempfile.mkdtemp(prefix="st-e2e-")
    env = {k: v for k, v in os.environ.items() if not k.startswith("SUPERTERMINAL")}
    env.update(HOME=home, XDG_CONFIG_HOME=os.path.join(home, ".config"), XDG_CACHE_HOME=os.path.join(home, ".cache"),
               TERM="xterm-256color", ZDOTDIR="", PS1="")
    env.pop("ZDOTDIR")
    env.pop("PS1")

    with open(os.path.join(home, ".zshenv"), "w") as f:
        # Ubuntu's /etc/zsh/zshrc runs compinit, which stops to ask about "insecure
        # directories" on machines like CI runners. Plain zsh would ask the same.
        f.write("skip_global_compinit=1\n")
        # For a zsh unpacked outside /usr: tell it where its modules are.
        if os.environ.get("ST_TEST_ZSH_MODULE_PATH"):
            f.write("module_path=(%s)\n" % os.environ["ST_TEST_ZSH_MODULE_PATH"])
    posix = shell != "pwsh"
    prompt = {"bash": "$", "zsh": "%", "pwsh": "PS "}[shell]
    sc = dict(
        plain='echo "st""-plain-ok"\r', multi1="if true; then\r", multi2='echo "st-multi""-ok"; fi\r',
        setvar='stvar=$((41 + 1)); echo "var=$stvar"\r', sleep="sleep 60; echo st-sleep-finished\r",
        after_cancel='echo "st""-after-cancel"\r', after_ctrl_c='echo "st""-after-ctrl-c"\r',
        touch="touch zz-st-e2e.tmp\r",
        still='test -f zz-st-e2e.tmp && echo "st-still""-there"; rm -f zz-st-e2e.tmp\r')
    if not posix:
        sc = dict(
            plain="echo ('st' + '-plain-ok')\r", multi1="if ($true) {\r", multi2="'st-multi' + '-ok' }\r",
            setvar='$stvar = 41 + 1; "var=$stvar"\r', sleep="Start-Sleep -Seconds 60; 'st-sleep-finished'\r",
            after_cancel="echo ('st' + '-after-cancel')\r", after_ctrl_c="echo ('st' + '-after-ctrl-c')\r",
            touch="$null = New-Item zz-st-e2e.tmp -Force\r",
            still="if (Test-Path zz-st-e2e.tmp) { 'st-still' + '-there' }; Remove-Item zz-st-e2e.tmp\r")

    os.chdir(repo)
    extra = ["--direct"] if "--direct" in args else []
    s = Session([exe, "--shell", shell] + extra, env)
    at = s.wait_for("SuperTerminal", 0, 40)
    check(at >= 0, f"banner appears ({shell})")
    at = s.wait_for(prompt, max(at, 0), 30)
    check(at >= 0, "shell prompt appears")
    s.idle(0.8)
    s.type(sc["plain"])
    at = max(s.wait_for("st-plain-ok\n", at, 20), at)
    check(at >= 0 and "st-plain-ok\n" in s.text(), "plain command runs")
    s.type(sc["multi1"])
    s.idle(0.6)
    s.type(sc["multi2"])
    p = s.wait_for("st-multi-ok\n", at, 20)
    check(p >= 0, "multi-line input continues and runs")
    at = max(p, at)
    s.type(sc["setvar"])
    p = s.wait_for("var=42", at, 20)
    check(p >= 0, "session state persists")
    at = max(p, at)

    if not offline:
        s.type("show me the files in this folder whose names start with CMakeL\r")
        p = s.wait_for("CMakeLists.txt", at, 60)
        check(p >= 0, "an English request became a command that ran by itself")
        at = max(p, at)
        s.idle(1.0)
        s.type("now just the count of them\r")
        p = s.wait_for("\n1\n", at, 60)
        check(p >= 0, "follow-up request is understood (count = 1)")
        at = max(p, at)
        s.idle(1.0)
        s.type(sc["touch"])
        s.idle(0.8)
        s.type("delete the file called zz-st-e2e.tmp\r")
        p = s.wait_for("Enter to run", at, 60)
        check(p >= 0, "a deleting command waits for confirmation")
        at = max(p, at)
        s.idle(0.8)
        s.type("\x1b")
        s.idle(1.2)
        s.type(sc["after_cancel"])
        p = s.wait_for("st-after-cancel\n", at, 20)
        check(p >= 0, "Esc cancelled it and the prompt still works")
        at = max(p, at)
        s.type(sc["still"])
        p = s.wait_for("st-still-there\n", at, 20)
        check(p >= 0, "the cancelled delete never ran (file still there)")
        at = max(p, at)
        s.type("dir /w\r")
        p = s.wait_for("third_party", at, 60)
        check(p >= 0, "'dir /w' is translated and lists the folder")
        at = max(p, at)
        s.idle(1.0)
    s.type(sc["sleep"])
    s.idle(2.5)
    s.type("\x03")
    # .NET on Unix discards keys typed while PowerShell recovers from Ctrl+C; wait a moment.
    deadline = time.time() + (4.0 if shell == "pwsh" else 1.0)
    while time.time() < deadline:
        s.pump(0.1)
    if shell == "pwsh":
        # PowerShell on Unix drops the first keys typed after a Ctrl+C (a .NET console
        # quirk, same without SuperTerminal); give it an empty line to swallow.
        s.type("\r")
        s.pump(1.5)
    s.type(sc["after_ctrl_c"])
    p = s.wait_for("st-after-ctrl-c\n", at, 15)
    check(p >= 0 and "\nst-sleep-finished\n" not in s.text(), "Ctrl+C stops a running command and the session carries on")
    if p < 0 and os.environ.get("ST_TEST_DEBUG"):
        print(repr(s.raw[-1200:]))
    s.idle(0.8)
    s.type("\x03")
    check(s.finish(20) == 0, "Ctrl+C at the prompt exits SuperTerminal")

    x = Session([exe, "--shell", shell], env)
    x.wait_for(prompt, x.wait_for("SuperTerminal", 0, 40), 30)
    x.idle(1.0)
    x.type("\x18")
    check(x.finish(20) == 0, "Ctrl+X at the prompt exits SuperTerminal")

    screen = s.text().lower().replace("claude.md", "").replace("anthropic_api_key", "")
    leaks = [w for w in ("claude", "opus", "sonnet", "haiku", "anthropic", "thinking", " llm") if w in screen]
    check(not leaks, "no mention of the model or AI on screen" + (f" (found {leaks})" if leaks else ""))

    with open(os.path.join(repo, "build", f"e2e-{shell}.txt"), "w", encoding="utf-8") as f:
        f.write(s.text())
    shutil.rmtree(home, ignore_errors=True)
    print(f"\n{failures} failure(s).")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
