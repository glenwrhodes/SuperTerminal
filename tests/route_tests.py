#!/usr/bin/env python3
"""bash/zsh routing and auto-run safety tests (no shell or network needed).

    python3 tests/route_tests.py <path to st>

Each case gives a line, what the shell reported about it (syntax check and
`type -t` for its words), and whether it should go to translation. The
PowerShell cases live in tests/route-tests.ps1 (they need pwsh for the parse).
"""
import json
import subprocess
import sys

# line, syntax, {word: type}, expected: True = translate, False = run as typed
ROUTE = [
    ("ls -la", "ok", {"ls": "alias"}, False),
    ("git status", "ok", {"git": "file"}, False),
    ("cd ..", "ok", {"cd": "builtin"}, False),
    ("echo hello world", "ok", {"echo": "builtin"}, False),
    ("x=5", "ok", {"x=5": "none"}, False),
    ("FOO=1 make build", "ok", {"FOO=1": "none", "make": "file"}, False),
    ("grep -r 'the thing in my code' .", "ok", {"grep": "file"}, False),
    ("ls | grep foo", "ok", {"ls": "alias", "grep": "file"}, False),
    ("if true; then", "incomplete", {"if": "keyword"}, False),
    ("echo 'unterminated", "incomplete", {"echo": "builtin"}, False),
    ("!! show me the files", "ok", {}, False),
    ("show me all the docx files in this folder", "ok", {"show": "none"}, True),
    ("Get-ChildItem -Recurse", "ok", {"Get-ChildItem": "none"}, True),
    ("dir /w", "ok", {"dir": "file"}, True),
    ("find all docx files", "ok", {"find": "file"}, True),
    ("sort these by size", "ok", {"sort": "file"}, True),
    ("where is python", "ok", {"where": "none"}, True),
    ("which files are bigger than 10 MB?", "ok", {"which": "file"}, True),
    ("what's in this folder", "incomplete", {}, True),
    ("fix it", "ok", {"fix": "none"}, True),
    ("gti status", "ok", {"gti": "none"}, True),
    ("?? ls", "ok", {"ls": "alias"}, True),
    ("ls -la )", "error", {"ls": "alias"}, True),
]

# command, risk, expected: may run without Enter
SAFETY = [
    ("ls -la", "read_only", True),
    ("du -sh * | sort -h", "read_only", True),
    ("find . -name '*.log'", "read_only", True),
    ("ls > /dev/null 2>&1", "read_only", True),
    ("git log --oneline -5", "read_only", True),
    ("ping -c 3 example.com", "read_only", True),
    ("cd ~/Downloads && ls", "read_only", True),
    ("find . -name '*.log' -delete", "read_only", False),
    ("rm -rf build", "read_only", False),  # mislabelled: the local check still catches it
    ("cat a.txt > b.txt", "read_only", False),
    ("echo $(rm x)", "read_only", False),
    ("git push", "read_only", False),
    ("sudo ls /root", "read_only", False),
    ("ping example.com", "read_only", False),
    ("rm notes.txt", "destructive", False),
    ("mkdir new", "modifies", False),
    ("sleep 100 &", "read_only", False),
]


def run(exe, flag, rows):
    payload = "\n".join(json.dumps(r) for r in rows) + "\n"
    out = subprocess.run([exe, flag], input=payload, capture_output=True, text=True, encoding="utf-8")
    return [json.loads(line) for line in out.stdout.splitlines() if line.strip()]


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else "build/st"
    failed = 0
    got = run(exe, "--route-test-posix",
              [{"line": l, "syntax": s, "types": t} for l, s, t, _ in ROUTE])
    for (line, _, _, want), g in zip(ROUTE, got):
        ok = bool(g.get("toAi")) == want
        failed += not ok
        print(f"{'PASS' if ok else 'FAIL'} route  {line:<45} -> {'translate' if g.get('toAi') else 'run':<9} {g.get('reason')}")
    got = run(exe, "--safety-test-posix", [{"command": c, "risk": r} for c, r, _ in SAFETY])
    for (cmd, _, want), g in zip(SAFETY, got):
        ok = bool(g.get("allowed")) == want
        failed += not ok
        print(f"{'PASS' if ok else 'FAIL'} auto   {cmd:<45} -> {'auto' if g.get('allowed') else 'ask':<9} {g.get('reason')}")
    total = len(ROUTE) + len(SAFETY)
    if len(got) != len(SAFETY):
        failed += 1
    print(f"\n{total - failed} passed, {failed} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
