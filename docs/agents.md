# wbsh and coding agents on Windows

How to point each coding agent at wbsh, what has been verified about each one
and how. Verified on **2026-11-12** with wbsh 1.1.1 (branch
`feat/drop-in-roadmap`) on Windows 10 22H2, by
`tools/agents/check_agent_shapes.py`.

No agent was run for this page. The harness rebuilds the command line,
environment, working directory and stdin each agent hands to bash, from what
is known about the agent, and checks that wbsh answers them the way bash
would. Where this page says *unverified*, the way the agent starts its shell
is not known well enough to emulate; treat those notes as a starting point,
not a promise.

## Setting wbsh up as bash

Most agents look for a `bash.exe`, so give them one: copy `wbsh.exe` to a
directory of its own under the name `bash.exe` (and `sh.exe` if something
wants `sh`), for example `C:\Tools\wbsh\bash.exe`. The copy behaves exactly
like `wbsh.exe`; one named `sh.exe` starts in POSIX mode. Inside the shell,
`$WBSH_VERSION` is set, so a shared `~/.bashrc` can tell the two apart:

```sh
if [ -n "$WBSH_VERSION" ]; then
    : # wbsh-only settings
fi
```

A login shell (`bash -l`, which several agents use) reads `~/.wbsh_profile`,
never `~/.bash_profile` or `~/.profile`, so Git Bash's profile does not leak
into agent runs.

## What the harness checks

```
python -I tools/agents/check_agent_shapes.py [--wbsh PATH] [--quick] [--shape NAME ...] [-v]
```

It copies wbsh to `<temp>\Program Files\Git\bin\bash.exe` (spaces in the
path, as with Git for Windows), works in a directory whose name has spaces and
non-ASCII letters, and runs every check against every shape. It never opens a
window (every process gets `CREATE_NO_WINDOW`) and exits non-zero on any
failure.

| Shape | Command line | Stands for |
| --- | --- | --- |
| `claude-code` | `bash -c -l "source SNAPSHOT && eval 'CMD' < /dev/null && pwd -P >\| CWDFILE"` | Claude Code |
| `codex` | `bash -lc CMD` | Codex CLI |
| `bash-c` | `bash -c CMD` | OpenCode, VS Code / Cursor shell tasks, Node's `spawn(cmd, {shell: bash})` |
| `ec` | `bash --ec BASE64` | any caller that can base64 |
| `script` | `bash C:\...\cmd.sh` | the fallback for commands too long for a command line |
| `stdin` | `bash -s < CMD` | the same fallback through a pipe |
| `cmd-ec` | `cmd /c "bash.exe --ec BASE64"` | Aider, and anything else using `shell=True` / cmd.exe |
| `json` | `bash --json -c CMD` | wbsh's structured result |

Checks, run for every shape where they apply:

- exit codes 0 to 255 through `exit N`; the last command's status; `exit 300`
  (44); an `EXIT` trap that changes the status;
- stdout and stderr arrive separately and complete; a bare `true` prints
  nothing at all (no banner, no prompt, no warnings on either stream);
- output order when the shell's own `echo`, a native program and a pipeline
  write in turn;
- a caller that leaves stdin open and never writes to it does not hang the
  run (except `stdin`, where stdin is the script);
- the working directory: spaces and non-ASCII names, an 8.3 short path given
  as the cwd (`pwd` answers the long name), Claude Code's `pwd -P >|` file
  after a `cd`, and that its `/c/...` form maps back to the Windows path;
- the environment, including Node's `Path` / `SystemRoot` spelling (Node keeps
  Windows' casing, Python upper-cases it);
- quoting round-trips of 18 hostile strings (quotes, `$`, backticks,
  backslashes, newlines, tabs, `!`, `%VAR%`, `^`, globs, operators, Czech,
  CJK and emoji, empty strings, `-n`, `--`, a trailing backslash), both inside
  the command and as `$1`, and as arguments to a native program (wbsh's own
  Windows command-line quoting);
- `"$BASH" -c` from inside a run (the shell's own path has spaces);
- commands of 9 KB and 40 KB: they must run where the command line can hold
  them, and are reported as `limit` where Windows cannot (32767 characters
  for CreateProcess, 8191 for cmd.exe);
- killing a run with `taskkill /T /F` ends the external grandchild too;
  killing only the shell is reported as information (see below);
- `--ec` in UTF-16LE (PowerShell's encoding); `--json` with bytes that are
  not UTF-8, NUL and control characters; `--json --ec`;
- Claude Code's shell snapshot (below) carries aliases, functions, `shopt` and
  `set -o` state from `~/.bashrc` into the commands, and sourcing it is silent.

### Results on 2026-11-12

```
                                  claude-code  codex  bash-c  ec     script  stdin  cmd-ec  json
exit 0..255                       ok           ok     ok      ok     ok      ok     ok      ok
last status, exit 300, EXIT trap  ok           ok     ok      ok     ok      ok     ok      ok
stdout/stderr separate            ok           ok     ok      ok     ok      ok     ok      ok
silent `true` (no banner/prompt)  ok           ok     ok      ok     ok      ok     ok      ok
output order, native + pipeline   ok           ok     ok      ok     ok      ok     ok      ok
stdin left open: no hang          ok           ok     ok      ok     ok      -      ok      ok
cwd (spaces, unicode)             ok           ok     ok      ok     ok      ok     ok      ok
cwd given as 8.3 short path       ok           ok     ok      ok     ok      ok     ok      ok
cwd tracking file (pwd -P)        ok           -      -       -      -       -      -       -
env passing                       ok           ok     ok      ok     ok      ok     ok      ok
nested "$BASH" -c                 ok           ok     ok      ok     ok      ok     ok      ok
quoting round-trip                ok           ok     ok      ok     ok      ok     ok      ok
argv operands round-trip          -            ok     ok      ok     ok      ok     -       ok
argv to native exe                ok           ok     ok      ok     ok      ok     ok      ok
9 KB command                      ok           ok     ok      ok     ok      ok     limit   ok
40 KB command                     limit        limit  limit   limit  ok      ok     limit   limit
kill tree (taskkill /T)           ok           ok     ok      ok     ok      ok     ok      ok
kill shell only                   ok           -      ok      -      -       -      -       -
--ec UTF-16LE                     -            -      -       ok     -       -      -       -
bare `bash` reading a pipe        -            -      -       -      -       ok     -       -
snapshot: alias/function/opts     ok           -      -       -      -       -      -       -
--json invalid UTF-8, NUL         -            -      -       -      -       -      -       ok
--json --ec                       -            -      -       -      -       -      -       ok
```

`kill shell only`: when a caller terminates just the shell process (Node's
`child.kill()`, Python's `Popen.kill()`), the foreground command and the
programs it started die with it, so a caller waiting for end-of-file is not
held up. wbsh keeps its foreground children in a kill-on-close job object
that a normal exit releases; `&`, `disown`, `nohup` and `setsid` children are
kept out of it and survive. Git Bash leaves the native program running.
`tools/agents/check_process_lifetime.py` checks both directions, and that a
console-less wbsh starts console programs without a window.

## Claude Code

**Setup.** Claude Code on Windows runs its Bash tool through Git Bash and
takes the path from `CLAUDE_CODE_GIT_BASH_PATH` (documented by Anthropic). Set
it as a user environment variable and restart Claude Code:

```
setx CLAUDE_CODE_GIT_BASH_PATH "C:\Tools\wbsh\bash.exe"
```

The `env` block of `~/.claude/settings.json` may also work; whether it is
applied before Claude Code looks for Git Bash is unverified.

**What it does** (reconstructed from Claude Code 1.x/2.x behaviour; details
vary between versions):

1. Once per session it runs a login shell (`bash -c -l`) that sources
   `~/.bashrc` and writes a *snapshot* file: every function as
   `eval "$(echo 'BASE64' | base64 -d)"` (from `declare -f`), `shopt -p`,
   `set -o NAME` for each option that is on, `shopt -s expand_aliases`,
   `alias -- name='value'` lines, and `export PATH=...`.
2. Each command then runs as
   `bash -c -l "source SNAPSHOT && eval 'COMMAND' < /dev/null && pwd -P >| CWDFILE"`,
   with paths in `/c/...` form, the environment of the Claude Code process
   (Windows casing, plus `CLAUDECODE=1`, `GIT_EDITOR=true`, `SHELL`), and the
   project directory as cwd. Claude Code reads the new working directory back
   from `CWDFILE`.
3. On timeout or interrupt it kills the process tree.

All three are covered by the `claude-code` shape. Unverified:

- how the stdin redirect is attached: some versions pass `\<` to `eval`
  (which attaches it to the last command of a pipeline, in bash and wbsh
  alike) or move it into the first pipeline stage; the harness redirects
  `eval` as a whole;
- whether Claude Code converts the cwd file's `/c/...` path itself or calls
  `cygpath.exe`; wbsh's `cygpath` is a builtin, not a program on PATH;
- whether the shell is spawned `detached` (no console). wbsh copes without
  a console, but native programs it starts could then each get a console
  window of their own; this was not tested because it opens windows.

Because the snapshot step sources `~/.bashrc` explicitly, Git-Bash-only lines
there run under wbsh; guard them with `$WBSH_VERSION` as above.

## OpenAI Codex CLI

**Setup.** Codex's shell tool runs the argument vector the model writes,
which for bash is `["bash", "-lc", "COMMAND"]`, and finds `bash` through
`PATH`. On Windows `C:\Windows\System32\bash.exe` (the WSL launcher) usually
comes first, because the system `PATH` precedes the user's. Either put the
directory holding wbsh's `bash.exe` ahead of `%SystemRoot%\system32` in the
system `PATH`, or start Codex from a shell whose `PATH` starts with it.

**Verified:** the `codex` shape (`bash -lc`). **Unverified:** current Codex
releases on Windows default to PowerShell, and whether the model is steered to
`bash -lc` there, or a setting selects the shell, is not known.

## OpenCode

**Setup.** OpenCode runs commands through a shell found on the system; on
Windows it looks for Git Bash. Put wbsh's `bash.exe` where it looks, or set
`SHELL` to it. The exact lookup order and any OpenCode-specific variable are
unverified.

**Verified:** the `bash-c` shape, which is what Node's
`spawn(command, { shell: "bash.exe" })` produces (`bash -c COMMAND`).

## Cursor and VS Code

**Tasks** of type `shell` run `SHELL -c "COMMAND"` with the automation
profile. Point it at wbsh in `settings.json`:

```json
"terminal.integrated.automationProfile.windows": {
    "path": "C:\\Tools\\wbsh\\bash.exe"
}
```

**Verified:** the `bash-c` shape.

**Not covered:** the agents in Cursor and in VS Code (Copilot agent mode)
type commands into an *interactive* integrated terminal with shell
integration. That is interactive wbsh, outside this harness. A terminal
profile makes wbsh that terminal:

```json
"terminal.integrated.profiles.windows": {
    "wbsh": { "path": "C:\\Tools\\wbsh\\wbsh.exe" }
},
"terminal.integrated.defaultProfile.windows": "wbsh"
```

## GitHub Copilot CLI, Gemini CLI

Both run commands through PowerShell or cmd.exe on Windows, not bash, as far
as is known (unverified for current releases). wbsh is reached only when the
command itself calls it. Through cmd.exe or PowerShell, use `--ec` so neither
layer touches the quoting, and `--json` for a result that needs no parsing:

```
wbsh --json --ec ZWNobyBoaQ==
```

**Verified:** the `cmd-ec` shape (cmd.exe in between), and `--ec` in
UTF-16LE as PowerShell's `[Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($cmd))`
produces it.

## Aider

`/run`, `/test` and lint commands go through `subprocess` with `shell=True`,
that is `cmd.exe /c` (or PowerShell when Aider runs under it). Aider never
starts bash itself; write the command as `wbsh --ec BASE64` or `wbsh FILE`.

**Verified:** the `cmd-ec` shape. A command through cmd.exe is limited to
8191 characters, so `--ec` holds about 6 KB of script; use a file beyond that.

## Continue

Unverified. The VS Code extension runs terminal commands in the integrated
terminal (see Cursor and VS Code); how the `cn` CLI starts a shell on Windows
is not known. If it spawns `bash -c`, the `bash-c` shape covers it.

## Limits every agent shares

- Windows caps a command line at 32767 characters (8191 through cmd.exe), so
  a 40 KB command cannot be passed as `-c` or `--ec` at all. Write it to a
  file and run `bash FILE`, or pipe it to `bash -s`.
- Script files, stdin and `--ec` commands may use CRLF line endings; they are
  read as LF.
- `bash --version` prints `wbsh X.Y.Z`, not GNU bash's banner;
  `$BASH_VERSION` reports the bash version wbsh targets.
