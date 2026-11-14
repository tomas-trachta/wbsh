---
name: wbsh
description: Run shell commands on Windows with wbsh, a Bash-compatible shell (often installed as bash.exe). Use when a task runs bash, sh or POSIX shell commands on Windows, when quoting a command for bash is error-prone, or when a structured result (status, stdout, stderr) is wanted.
---

# wbsh on Windows

wbsh is a Bash-compatible shell for Windows: bash's grammar, builtins and
command line, plus a large set of built-in Unix commands (`ls`, `grep`,
`sed`, `awk`, `find`, `xargs`, `tar`, `curl`, `make`, …), in one native exe. It
may be installed as `wbsh.exe`, or copied as `bash.exe` / `sh.exe` (the
`sh` name starts in POSIX mode). `agent-info.txt` beside this file is the
shell's own brief; `commands.txt` lists every built-in command.

## Running a command

Use it exactly like bash:

```
wbsh -c 'grep -rn TODO src | head -20'
wbsh -lc 'make test'                  # -l: login shell, reads ~/.wbsh_profile
wbsh -eu -o pipefail -c '...'         # errexit, nounset, pipefail
wbsh -c 'printf "%s\n" "$1"' name arg # $0 = name, $1 = arg
wbsh ./script.sh a b
```

A non-interactive run prints no banner or prompt, reads no rc file, writes
LF line endings, and keeps stdout and stderr apart. The working directory
and the environment come from the caller.

### Quote-proof: `--ec`

Every layer between you and the shell (cmd.exe, PowerShell, a JSON tool
call, a process API) can re-quote `$`, `"`, `\`, `!` or `%`. Send the
command base64-encoded instead and nothing needs escaping:

```
wbsh --ec ZWNobyAiaGkgJDEi name arg   # runs: echo "hi $1"
```

UTF-8 or UTF-16LE (PowerShell's `-EncodedCommand` form) both work; padding
and the URL-safe alphabet are accepted. Options go before it:
`wbsh -eu --ec <b64>`. Encoders:

| Language | Encoding a command `cmd` |
| --- | --- |
| Python | `base64.b64encode(cmd.encode()).decode()` |
| Node | `Buffer.from(cmd, 'utf8').toString('base64')` |
| PowerShell | `[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($cmd))` |
| C# | `Convert.ToBase64String(Encoding.UTF8.GetBytes(cmd))` |
| Go | `base64.StdEncoding.EncodeToString([]byte(cmd))` |
| Rust | `base64::engine::general_purpose::STANDARD.encode(cmd)` |

### Structured result: `--json`

```
wbsh --json -c 'echo hi; echo oops >&2; exit 3'
{"status":3,"stdout":"hi\n","stderr":"oops\n","duration_ms":4}
```

One line of pure-ASCII JSON (non-ASCII as `\uXXXX`; bytes that are not
UTF-8 become U+FFFD). The command runs as a normal top-level script (traps,
`exit N`, `set -e`), and the exit status is the command's. Combine it with
`--ec` (`wbsh --json --ec <b64>`) to need neither quoting nor output
parsing. A run killed from outside prints nothing.

### Long commands

Windows caps a command line at 32767 characters, and cmd.exe at 8191. For
anything longer, write the command to a file and run `wbsh FILE`, or pipe
it to `wbsh -s`. Script files and stdin may use CRLF line endings.

## Paths

- Inside the shell, paths are POSIX style: `C:\Users\me` is `/c/Users/me`,
  `$HOME` is `/c/Users/me`, `pwd` prints `/c/...`. Built-in commands accept
  either form. `cygpath -w /c/x` and `cygpath -u 'C:\x'` convert.
- Arguments passed to native Windows programs (git, node, python, cmd) are
  converted by Git Bash's rules: `/c/x`, `--opt=/c/x` and `/c/a:/d/b` lists
  become `C:\x`, `--opt=C:\x`, `C:\a;D:\b`; `/tmp/x` becomes `%TEMP%\x`;
  `/dev/null` becomes `nul`. URLs, `./rel`, `a:b` and anything with `*`, `?`,
  `[`, `]`, quotes or `;` pass unchanged.
- `//c` passes a literal `/c` (`cmd //c dir`, `taskkill //F //PID 42`);
  a two-character switch such as `cmd /c` also works unescaped in wbsh.
- `MSYS_NO_PATHCONV=1` turns the conversion off; `MSYS2_ARG_CONV_EXCL` takes
  `*` or `;`-separated prefixes to leave alone.
- There is no MSYS root: `/usr/bin`, `/etc`, `/opt` do not exist.

## Exit status

`$?` and the process exit code follow bash: 0–255, `exit 300` gives 44,
126 / 127 for "cannot execute" / "command not found", 128+n for a signal
(130 Ctrl-C, 143 TERM). A native program that crashes reports 139 (like a
segfault). `timeout` returns 124. `set -o pipefail` is honoured.

## Commands

`commands.txt` lists the built-in commands; inside the shell, `help` lists
them and `type -a NAME` says whether a name is a builtin or a program on
PATH. `git`, `python`, `node` and the like come from PATH as ordinary
Windows programs.

## Where it differs from GNU bash

Most scripts run unchanged. Known gaps (tools/difftest/FINDINGS.md in the
source tree has the full list with reproducers):

- Error messages lack bash's `line N:` prefix in places; don't parse them.
- Aliases expand in scripts without `shopt -s expand_aliases`.
- `failglob`, `GLOBIGNORE` and `nocasematch` in `case` are not honoured;
  `**` recurses without `globstar`.
- Some array corners (`a[-1]=x`, `s+=(arr)`, `unset 'a[@]'`) and
  `mapfile -d` differ or are missing.
- `shopt -s lastpipe` has no effect.
- `exec PROGRAM` runs the program and exits with its status (Windows has
  no `execve`).
- A login shell reads `~/.wbsh_profile`, never `~/.bash_profile` or
  `~/.profile`; an interactive one reads `~/.wbshrc`.
- `bash --version` prints `wbsh X.Y.Z`; `$BASH_VERSION` reports the bash
  version wbsh targets.
- Killing only the shell process leaves its child programs running, as
  with Git Bash; kill the whole tree (`taskkill /T /F /PID n` or a job
  object) on timeout.
