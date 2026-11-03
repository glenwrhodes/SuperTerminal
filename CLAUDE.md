# SuperTerminal: notes for coding agents

A C++20 terminal host (`st`) that runs the user's real shell in a pseudo terminal and turns plain-English lines into commands through the Claude Messages API. Read `README.md` for the design.

- **Self-contained by design.** `st` calls `api.anthropic.com` directly with the user's `ANTHROPIC_API_KEY`. Never add a dependency on a hosted service, a runtime (Python/Node/.NET) or a server.
- **Never reveal the model in the UI.** The screen shows only the command or the answer: no model names, timings, "AI", "Claude" or "thinking". The end-to-end tests check this.
- **Models:** only `claude-haiku-5-5` (default), `claude-sonnet-5-5`, `claude-opus-5-5`. A decline retries on the other two; never use server-side `fallbacks` (they can answer on older models). API version header `2026-10-21`; structured output via `output_config.format`.
- **Build and verify:** `pwsh -File build.ps1 -Test` (Windows) or `./build.sh --test` (macOS/Linux). Add `-Live` / `--live` for real requests. CI runs Windows, Linux and macOS on every push.
- **Two halves per shell:** C++ in `src/` and the integration scripts in `shell/` (compiled in by `cmake/embed.cmake`). Change both sides of the protocol together. Request types: hello, route, ask, check, mark, config. bash/zsh reach `st` through `st --client <pipe> <type>`, with fields on stdin as `key\x1fvalue\x1e`.
- **Gotchas found the hard way:**
  - PowerShell's `ConvertTo-Json` writes `$null` as `null`: read request fields with `jstr/jint/jbool` (`src/json_util.h`), never `json::value()`, which throws.
  - In the PowerShell shim, `@($genericList)` can throw "Argument types do not match": use `.ToArray()`.
  - bash 5.2: `$(<file)` inside a `bind -x` function breaks alias expansion of the next command line. Use `read -rd ''`. Command substitutions in `PS0` hit the same class of bug.
  - bash: `stty` changes inside `bind -x` are undone when readline accepts the line. Ctrl+C-to-exit is an INT trap installed at the prompt and removed in the Enter hook instead.
  - bash and zsh treat Esc as a meta prefix: bind it explicitly (emacs keymap only), or the next keystroke merges into a suggested command.
  - zsh `-n` reports unfinished input as "parse error near `then'": the token decides incomplete versus error.
  - `SetConsoleCtrlHandler(nullptr, TRUE)` is inherited by child processes: use a handler function.
  - Windows filenames are case-insensitive: `CMake*` matches `cmake/` too.
  - PowerShell on Unix asks the terminal for the cursor position (`ESC[6n`) and blocks until it's answered: test harnesses must answer it, including while "sleeping".
  - Don't pipe `st_e2e.exe` into `Select-Object -First`: the test keeps running and locks `st.exe`.
