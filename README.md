<p align="center">
  <img src="docs/splash.jpg" alt="SuperTerminal" width="820">
</p>

<p align="center">
  <b>Your shell, fluent in plain English.</b><br>
  Type <code>git status</code> and it runs. Type <i>"show me the 10 biggest files in here"</i> and that runs too.
</p>

<p align="center">
  <a href="https://github.com/glenwrhodes/SuperTerminal/actions/workflows/ci.yml"><img src="https://github.com/glenwrhodes/SuperTerminal/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
  <a href="https://github.com/glenwrhodes/SuperTerminal/releases"><img src="https://img.shields.io/github/v/release/glenwrhodes/SuperTerminal?include_prereleases&label=release" alt="Release"></a>
  <img src="https://img.shields.io/badge/platforms-Windows%20%7C%20macOS%20%7C%20Linux%20%7C%20WSL-blue" alt="Platforms">
  <img src="https://img.shields.io/badge/shells-PowerShell%20%7C%20bash%20%7C%20zsh-blueviolet" alt="Shells">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-green" alt="MIT"></a>
</p>

---

SuperTerminal is a single small C++ binary, `st`, that runs **your real shell** (PowerShell, bash, zsh, or bash inside WSL) and adds one superpower: anything you type that isn't a command gets turned into one.

```text
~/projects/site $ show me the 5 biggest files under src
~/projects/site $ find src -type f -exec du -h {} + | sort -rh | head -5
4.1M  src/assets/hero.mp4
812K  src/assets/fonts/Inter.woff2
 96K  src/app.js
...
~/projects/site $ now only the javascript ones
~/projects/site $ find src -type f -name '*.js' -exec du -h {} + | sort -rh | head -5
...
~/projects/site $ delete the .DS_Store files everywhere in here
  This deletes or overwrites. Enter to run, Esc to cancel.
~/projects/site $ find . -name .DS_Store -delete█
```

No chat window, no copy-paste, no new commands to learn. You see the real command and output, and you learn as you go.

## Why you'll like it

- **It's still your shell.** Your profile, aliases, prompt (oh-my-posh, starship, p10k), history, tab completion and every valid command work exactly as before. Valid commands never leave your machine.
- **Plain English just works.** "what's using port 3000", "unzip every archive in downloads", "how much disk space do I have left". It understands follow-ups too: "now sort them by size", "only the big ones".
- **"fix it" works.** After a command fails, type `fix it`. SuperTerminal looks at what you ran, the error and the output, and writes the corrected command.
- **Your old habits work anywhere.** `dir /w` on a Mac, `ls -la` and `rm -rf` in PowerShell, `Get-ChildItem` in bash: each one becomes the right command for the shell you're in.
- **Safe by design.** Commands that only read things run straight away. Anything that changes or deletes waits on your prompt for Enter, and Esc cancels it. Auto-run takes two independent yeses: the translation must say it's read-only, *and* a local parse of the command must agree (read-only programs only, no redirection into files, no `sudo`, no `-delete`).
- **Full-screen apps work.** `vim`, `htop`, `ssh`, `claude`, `less`: it's a real pseudo-terminal, so arrow keys, Ctrl combinations and mouse-free TUIs behave normally.
- **One binary, no runtime.** Windows, macOS, Linux and WSL. No Python, no Node, no server, no account. Bring your own API key.

## Install

**Download** a binary from [Releases](https://github.com/glenwrhodes/SuperTerminal/releases) and put it on your PATH, or **build from source** (about a minute):

```powershell
# Windows: Visual Studio 2022 or its Build Tools (C++ workload)
git clone https://github.com/glenwrhodes/SuperTerminal; cd SuperTerminal
pwsh -File build.ps1 -Install     # builds, puts st.exe on PATH, adds Windows Terminal profiles
```

```bash
# macOS / Linux: a C++20 compiler, CMake and libcurl headers (build.sh tells you how to get them)
git clone https://github.com/glenwrhodes/SuperTerminal && cd SuperTerminal
./build.sh --install              # builds and copies st to ~/.local/bin
```

Then set your key once ([get one here](https://console.anthropic.com/settings/keys)):

```bash
export ANTHROPIC_API_KEY=sk-ant-...                          # macOS / Linux: add to ~/.zshrc or ~/.bashrc
setx ANTHROPIC_API_KEY sk-ant-...                            # Windows (or System Properties > Environment Variables)
```

The macOS release is a universal binary (Apple silicon and Intel), signed with a Developer ID and notarized by Apple, so it runs without Gatekeeper warnings.

## Use it

```bash
st                       # your default shell: PowerShell on Windows, your login shell elsewhere
st --shell zsh           # or bash, pwsh, powershell
st --shell wsl           # Windows: bash inside WSL (or wsl:Ubuntu for a specific distro)
```

| You type | What happens |
| --- | --- |
| any real command | runs exactly as typed, with no delay and no network |
| plain English | becomes a command. Read-only ones run; anything else waits for Enter or Esc |
| a question ("why did that fail?") | a short answer, printed right there |
| `fix it` | repairs the last failed command |
| `?? <text>` | always treat the line as English |
| `!! <command>` | always run as typed |
| `st help` / `st status` | help and settings |
| `st auto off` | make every translated command wait for Enter |
| Ctrl+C / Ctrl+X at the prompt | exit SuperTerminal (Ctrl+C in a running program still just stops it) |

**Windows Terminal:** `st --install-terminal-profile` adds "SuperTerminal" (and "SuperTerminal (WSL)") to the new-tab dropdown.

**Running as administrator:** an elevated `st` starts an elevated shell. WSL still logs in as your normal Linux user.

## Shells and platforms

| | PowerShell 7 | Windows PowerShell 5.1 | bash 4+ | zsh | WSL bash |
| --- | :-: | :-: | :-: | :-: | :-: |
| Windows | ✅ default | ✅ | (via WSL) | (via WSL) | ✅ `--shell wsl` |
| macOS | ✅ | | ✅ (Homebrew bash) | ✅ default | |
| Linux | ✅ | | ✅ default | ✅ | |

cmd.exe isn't supported: it has no way to hook the Enter key. Use PowerShell, which understands most cmd habits anyway.

## How it works

```text
 your terminal (Windows Terminal, iTerm2, GNOME Terminal, ...)
        │  keys in, output out
        ▼
 st ── pseudo terminal (ConPTY / forkpty) ── your shell + a small integration script
  │  ▲                                              │
  │  └────── local socket / named pipe ◄────────────┘  hooks Enter
  ▼
 api.anthropic.com   (only for lines that aren't commands)
```

1. **st hosts your real shell** in a pseudo terminal and relays it. Output passes through untouched (a rolling copy lets "fix it" see what happened), and keys pass in with full fidelity.
2. **A tiny integration script hooks Enter** (PSReadLine for PowerShell, readline for bash, ZLE for zsh). It asks the shell itself whether the line is runnable: PowerShell's parser with `Get-Command` and parameter checks, or `bash -n` / `zsh -n` with `type -t` for every word. **Nothing runs during the check.** That's how `del everything older than a week` never reaches `Remove-Item`.
3. **The router decides.** Unknown command, broken syntax, parameters the shell would reject (`ls -la` in PowerShell), cmd habits (`dir /w`), or valid-but-English (`sort these by size`, `where is python`) get translated. Everything else runs instantly.
4. **The translation** gets your OS, shell, current folder and its listing, recent commands with their exit status, the last command's output, and your previous requests, so follow-ups make sense. It comes back as structured JSON: a command or an answer, plus a risk level.
5. **The command lands on your prompt** as a normal line in your history, then runs, or waits for you.

### Privacy

Only lines that go to translation are sent anywhere. With each one goes the context above: file *names* in the current folder (not contents), your last few commands, and the tail of the last command's output. Real commands never leave your machine. The API key is read from your environment and only ever sent to `api.anthropic.com`.

## Configuration

Settings live in `~/.config/superterminal/settings.json` (Windows: `%APPDATA%\SuperTerminal\settings.json`).

| Setting | Default | Change with |
| --- | --- | --- |
| model | `haiku` (fastest) | `st model sonnet` / `st model opus`, or `SUPERTERMINAL_MODEL` for one session |
| effort | `low` | `st effort medium` (smarter, slower) |
| auto-run read-only commands | on | `st auto off` |
| default shell | platform default | `"shell": "zsh"` in settings.json, or `--shell` |

`st --ask "list my docker containers" --model sonnet` translates one request from the command line, which is handy for comparing models.

## Building and testing

```bash
./build.sh --test             # macOS / Linux: routing + safety tests, end-to-end tests in every installed shell
pwsh -File build.ps1 -Test    # Windows: same, plus WSL if a distro is installed
#   add --live / -Live to include real translation requests (uses your key; costs a fraction of a cent)
```

The end-to-end tests run `st` in a pseudo terminal and type at it like a person: plain commands, multi-line input, session state, English → command, follow-ups, confirm and cancel (checking the file really survived), `dir /w`, Ctrl+C into a running command, and exiting. CI runs them on Windows, Linux and macOS for every push.

| Path | What |
| --- | --- |
| `src/main.cpp` | options, shell selection, launch, `--client` mode for the bash/zsh hooks |
| `src/brain.*`, `src/router*.cpp`, `src/safety.cpp` | routing, translation requests, auto-run safety (PowerShell and POSIX) |
| `src/win/`, `src/unix/` | pseudo terminal host, IPC and HTTPS for each platform |
| `shell/superterminal.{ps1,bash,zsh}` | the shell integrations, compiled into the binary |
| `tests/` | routing/safety cases, Windows and Unix end-to-end harnesses |

## Known limitations

- **cmd.exe** isn't supported (no Enter hook). **macOS's built-in bash 3.2** can't host the integration; use zsh (the macOS default) or Homebrew bash.
- **PowerShell on macOS/Linux** drops the first few keys typed right after a Ctrl+C. That's a .NET console quirk that happens without SuperTerminal too.
- Lines that *look* like English but are real commands with unquoted English arguments (`claude fix the bug`) get translated. Quote the argument or use `!!`.

## Contributing

Issues and PRs are welcome. Run the tests before opening a PR (`./build.sh --test` or `pwsh -File build.ps1 -Test`). Routing changes need a case in `tests/route-tests.ps1` (PowerShell) or `tests/route_tests.py` (bash/zsh).

## License

[MIT](LICENSE) © 2026 Glen Rhodes. Uses [nlohmann/json](https://github.com/nlohmann/json) (MIT).
