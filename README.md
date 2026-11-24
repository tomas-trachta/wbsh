# wbsh

**A fast, native Bash for Windows.** One 4 MB executable: no MSYS, no Cygwin,
no WSL. Your scripts, git hooks and coding agents get a real Bash, and you get
a modern line editor and about 180 built-in commands from the first launch.

[![CI](https://github.com/tomas-trachta/wbsh/actions/workflows/ci.yml/badge.svg)](https://github.com/tomas-trachta/wbsh/actions/workflows/ci.yml)
[![Latest release](https://img.shields.io/github/v/release/tomas-trachta/wbsh)](https://github.com/tomas-trachta/wbsh/releases/latest)
[![License: MIT](https://img.shields.io/github/license/tomas-trachta/wbsh)](./LICENSE)

![wbsh in wbshterm, its companion terminal](./preview_terminal.png)

- **Fast where Git Bash is slow.** There is no `fork()` emulation to pay for:
  `$(...)` in a loop runs **30× faster** than in Git Bash, `while read` loops
  5.5×, startup 2× ([benchmark](docs/bench/2026-11-14-local.md), same machine).
- **Real Bash, measured.** Its own lexer, parser and expander in C++17, not a
  wrapper. It passes **68 of GNU bash 5.3's 86 upstream test suites** byte for
  byte; the other 18 differ only where Windows lacks a Unix path or where
  wbsh differs on purpose ([ledger](docs/compat-ledger.md)).
- **Batteries included.** `ls`, `grep`, `sed`, `awk`, `find`, `tar`, `curl`,
  `make`, `vi`, `tmux`, `fzf` and the rest are built in. Your installed
  `git`, `vim` and `ssh` are found automatically.
- **Works where Git Bash is expected.** A copy named `bash.exe` can stand in
  for Git Bash under git hooks, Makefiles, editor terminals and coding agents
  such as Claude Code and Codex CLI ([how, and what is verified](docs/agents.md)).
- **Your framework loads.** oh-my-bash, bash-it, bash-completion, nvm,
  sdkman, asdf and git's completion source cleanly in **968 of 1,034**
  runs, against Git Bash's 969 ([corpus](docs/corpus/2026-11-24-local.md)).
- **A pleasant prompt.** Syntax highlighting as you type, history
  predictions as ghost text, git status in the prompt, completion for git,
  docker, npm, cargo and kubectl.

wbsh is young: 1.x releases, CI on every push, and compatibility that is
measured rather than claimed. [Limitations](#limitations) lists what does
not work yet.

---

## Install

Run the installer from the [latest release](https://github.com/tomas-trachta/wbsh/releases/latest).
It installs for your user only (no admin prompt), adds wbsh to `PATH`, and
adds "Open wbsh here" to Explorer and a wbsh profile to Windows Terminal.

| | x64 | ARM64 |
| --- | --- | --- |
| **wbsh**, the shell | [wbsh-setup-x64.exe](https://github.com/tomas-trachta/wbsh/releases/latest/download/wbsh-setup-x64.exe) | [wbsh-setup-arm64.exe](https://github.com/tomas-trachta/wbsh/releases/latest/download/wbsh-setup-arm64.exe) |
| **wbshterm**, the terminal (includes wbsh) | [wbshterm-setup-x64.exe](https://github.com/tomas-trachta/wbsh/releases/latest/download/wbshterm-setup-x64.exe) | [wbshterm-setup-arm64.exe](https://github.com/tomas-trachta/wbsh/releases/latest/download/wbshterm-setup-arm64.exe) |

Or with [Scoop](https://scoop.sh):

```powershell
scoop install https://raw.githubusercontent.com/tomas-trachta/wbsh/main/packaging/scoop/wbsh.json
```

<details>
<summary>Portable ZIPs, installer options, winget, signing, ARM64</summary>

- **Portable:** `wbsh-<version>-portable-x64.zip` and
  `wbshterm-<version>-portable-x64.zip` (and `-arm64.zip` twins) on the
  release page. Extract and run; no registry, no `PATH` changes. For the
  Windows Terminal profile, copy `installer/windows-terminal/wbsh.json` to
  `%LOCALAPPDATA%\Microsoft\Windows Terminal\Fragments\wbsh\`.
- **Installer tasks:**

  | task | default |
  | --- | --- |
  | Add wbsh to the user `PATH` | on |
  | "Open wbsh here" in the Explorer context menu | on |
  | A wbsh profile for Windows Terminal (a fragment in `%LOCALAPPDATA%\Microsoft\Windows Terminal\Fragments\wbsh\`; `settings.json` is not touched) | on |
  | `bash.exe` and `sh.exe` shims in `{app}\bin`, added to the user `PATH`, for tools that look for Git Bash | off |
  | Set `SHELL` to wbsh for your user | off |
  | Install MinGit with winget, offered only when no `git.exe` is found | off |

- **winget:** manifests are generated with every release (see
  [packaging/README.md](packaging/README.md)); they become
  `winget install TomasTrachta.wbsh` once accepted into `microsoft/winget-pkgs`.
- **Checksums and signing:** every release from 1.1.2 on carries
  `SHA256SUMS.txt`. Releases are not code-signed yet, so SmartScreen may warn
  on first run; the signing step is prepared and switches on once the
  SignPath Foundation account exists ([docs/signing.md](docs/signing.md)).
- **ARM64:** the ARM64 builds are cross-compiled in CI and have not yet been
  run on ARM64 hardware by the maintainer; reports welcome.

</details>

[docs/editors.md](docs/editors.md) shows how to make wbsh the shell of
VS Code, Cursor, JetBrains IDEs and Windows Terminal.

---

## Quick start

```sh
$ wbsh
trach@DESKTOP /c/Users/trach (main)$ ls
Documents  Downloads  Desktop  ...
```

Run a command or a script exactly as with bash:

```sh
$ wbsh -c 'for i in {1..3}; do printf "%02d\n" "$((i * 7))"; done'
07
14
21

$ wbsh -eu -o pipefail script.sh arg1 arg2
```

Pipelines, redirection, control flow, functions, command substitution
(including bash 5.3's `${ cmd; }` and `${| cmd; }`), arithmetic, brace,
parameter, tilde and glob expansion, here-docs, here-strings, traps and job
control (`Ctrl-Z`, `jobs`, `fg`, `bg`) all work.

Paths translate the way Git Bash translates them: `/c/Users/...` becomes
`C:\Users\...` when a native Windows program is started, by Git Bash's rules
(`cmd //c`, `--opt=/c/x`, `MSYS2_ARG_CONV_EXCL`).

Settings go in `~/.wbshrc` (and `~/.wbsh_profile` for login shells). wbsh
never reads `~/.bashrc` or `~/.profile`, so an existing Git Bash setup is left
alone:

```sh
export EDITOR=code
alias ll='ls -lah'
alias gs='git status'
PS1='\[\e[35;1m\]\w\[\e[0m\]\g \$ '
```

### Using it as bash.exe

The command line is bash's (`-c`, `-l`, `-o`, `--norc`, `--posix`, clustered
letters such as `-ec`). A copy of `wbsh.exe` named `bash.exe` behaves the
same, and one named `sh.exe` starts in POSIX mode, so anything that runs
`bash -c` works unchanged. For Claude Code, set
`CLAUDE_CODE_GIT_BASH_PATH=C:\path\to\bash.exe`.

Two options are there for programs that call wbsh: `--ec` takes the command
as base64 so nothing needs escaping, and `--json` prints the status, stdout
and stderr as one JSON line:

```sh
$ wbsh --json -c 'echo hi; echo oops >&2; exit 3'
{"status":3,"stdout":"hi\n","stderr":"oops\n","duration_ms":4}
```

### Further reading

| | |
| --- | --- |
| [Command line](docs/command-line.md) | every option, `bash.exe` / `sh.exe` copies, `--ec`, `--json` |
| [Line editing](docs/line-editing.md) | history, predictions, completion, syntax highlighting, every key binding, `~/.inputrc` |
| [Configuration](docs/configuration.md) | startup files, variables, prompt escapes |
| [Commands](docs/commands.md) | flags and Windows notes for every built-in command |
| [Coding agents](docs/agents.md) | Claude Code, Codex CLI, OpenCode and others on wbsh |
| [Differences from POSIX](docs/posix-differences.md) | processes, signals, permissions, file system |

The same pages are published at
[tomas-trachta.github.io/wbsh](https://tomas-trachta.github.io/wbsh/).

---

## Scoreboard

Measured, not claimed. Each number links to the committed raw data; the CI
job that reproduces it is in [`.github/workflows/ci.yml`](.github/workflows/ci.yml).

| metric | wbsh | data | CI job |
| --- | --- | --- | --- |
| Speed vs Git Bash (same machine) | faster on every case: `$(...)` loop 39 vs 1,175 ms (30×), `while read` 88 vs 483 ms (5.5×), built-in pipeline 344 vs 1,265 ms (3.7×), `-c true` 24 vs 51 ms (2.1×) | [docs/bench/](docs/bench/2026-11-14-local.md) | `bench` (perf gate) |
| GNU bash 5.3 upstream suites | **68 / 86** byte-identical, 18 env-only, no real failures (2026-11-23) | [compat-ledger.md](docs/compat-ledger.md) | `bashtests` |
| Differential corpus vs GNU bash | 420 / 426 cases identical (vs bash 4.4, 2026-11-12); CI floor 400 vs bash 5.3 | [FINDINGS.md](tools/difftest/FINDINGS.md), [baseline.txt](tools/difftest/baseline.txt) | `difftest` |
| Real-world assets sourced cleanly | **968 / 1,034** runs (93.6 %; Git Bash 969) over oh-my-bash, bash-it, bash-completion, nvm, sdkman, asdf and git's completion scripts (2026-11-24) | [corpus snapshot](docs/corpus/2026-11-24-local.md) | local only |

[docs/comparison.md](docs/comparison.md) puts these next to Git Bash,
MSYS2 and WSL, with every cell sourced and dated.

---

## wbshterm

A terminal that hosts wbsh on a pseudoconsole, with tabs, tmux-style panes,
themes, a config file, scrollback and selection, a status bar and shell
integration. It bundles `wbsh.exe`, so it works on its own. See
[wbshterm/README.md](wbshterm/README.md).

---

## Commands

Everything below is built in. [docs/commands.md](docs/commands.md) has flags,
per-command notes, and what wbsh does where Windows differs from Linux: `exec`,
signals, `/proc`, FIFOs, `nohup`, cron and more.

**Shell:** `:`, `true`, `false`, `echo`, `printf`, `exec`, `pwd`, `cd`, `exit`,
`logout`, `return`, `break`, `continue`, `export`, `unset`, `shift`, `set`,
`eval`, `source` / `.`, `type`, `command`, `builtin`, `enable`, `read`, `test` /
`[`, `local`, `alias`, `unalias`, `history`, `fc`, `bind`, `trap`, `getopts`,
`declare` / `typeset`, `mapfile` / `readarray`, `shopt`, `let`, `umask`,
`ulimit`, `hash`, `times`, `caller`, `help`, `compgen`, `complete`, `compopt`,
`readonly`, `jobs`, `wait`, `fg`, `bg`, `disown`, `suspend`, `pushd`, `popd`,
`dirs`.

**Files:** `ls`, `cat`, `cp`, `mv`, `rm`, `mkdir`, `mkfifo`, `mknod`, `rmdir`,
`touch`, `ln`, `link`, `unlink`, `stat`, `chmod`, `chown`, `chgrp`, `chattr`,
`lsattr`, `install`, `file`, `du`, `df`, `realpath`, `readlink`, `basename`,
`dirname`, `find`, `dd`, `split`, `truncate`, `shred`, `sync`, `fsync`,
`flock`, `mktemp`.

**Text:** `head`, `tail`, `wc`, `sort`, `uniq`, `tr`, `cut`, `tee`, `paste`,
`join`, `tac`, `rev`, `nl`, `fold`, `column`, `expand`, `unexpand`, `comm`,
`shuf`, `tsort`, `strings`, `grep` / `egrep` / `fgrep`, `sed`, `awk` / `gawk`,
`diff`, `cmp`, `patch`, `dos2unix`, `unix2dos`, `iconv`, `xargs`, `less`,
`more`, `vi` / `view`, `ed`.

**Encoding and hashing:** `base64`, `base32`, `uuencode`, `uudecode`, `xxd`,
`od`, `hexdump`, `hd`, `md5sum`, `sha1sum`, `sha256sum`, `sha384sum`,
`sha512sum`, `sha3sum`, `cksum`, `sum`, `crc32`.

**Archives and compression:** `tar`, `gzip`, `gunzip`, `zcat`, `bzip2`,
`bunzip2`, `bzcat`, `xz`, `unxz`, `xzcat`, `lzma`, `unlzma`, `lzcat`, `lzop`,
`unlzop`, `lzopcat`, `uncompress`, `zip`, `unzip`, `cpio`, `ar`.

**Processes and system:** `ps`, `pgrep`, `pkill`, `pidof`, `kill`, `killall`,
`top`, `nice`, `renice`, `nohup`, `setsid`, `timeout`, `watch`, `script`,
`unbuffer`, `crontab`, `crond`, `uptime`, `free`, `nproc`, `env`, `printenv`,
`whoami`, `logname`, `groups`, `id`, `su`, `sudo`, `hostname`, `uname`, `date`,
`cal`, `sleep`, `usleep`, `stty`, `reset`, `tput`, `ttysize`, `clear`,
`locale`.

**Network:** `curl`, `wget`, `nc` / `netcat`, `ftp`, `ftpget`, `ftpput`,
`whois`, `httpd`.

**Git Bash helpers:** `cygpath`, `start`, `winpty`.

**Other:** `seq`, `expr`, `bc`, `dc`, `factor`, `getopt`, `uuidgen`, `ascii`,
`ipcalc`, `ts`, `yes`, `which`, `make` / `gmake`, `man`, `apropos`, `whatis`,
`fzf`, `tmux`, `utils`.

`git`, `vim`, `less`, `ssh` and the like are expected on PATH; `git` is
auto-discovered from the standard install locations.

---

## Third-party utils

A util is a DLL dropped into `plugins`: its commands join the shell and its
segments join the terminal's status bar, with no rebuild or registration.

```
$ utils
hello            1.0.0      A worked example: one command and one status segment.
$ hello Tomas | tr a-z A-Z
HELLO, TOMAS!
```

| Folder | For |
| --- | --- |
| `<install>\plugins\` | Utils that ship with an install |
| `%APPDATA%\wbsh\plugins\` | A user's own, no administrator needed |
| each folder in `WBSH_PLUGINS` | A developer's own build output |

- **Commands:** a util's command redirects, pipes and sets `$?` like any other,
  has `help` text and Tab completion of its own, and cannot take a bundled
  command's name.
- **Contract:** C (`sdk/include/wbshsdk.h`), ABI-versioned and unlinked. The
  host hands a util a table of functions when it loads, so any compiler that
  makes a DLL will do.
- **Failures:** a mismatched or non-util DLL is named and skipped without
  affecting the others, and `utils` says why.
- **Development:** `utils load <dll>` and `utils reload` keep the edit-run loop
  inside one shell.
- **Optional:** `wbsh.exe` reaches the SDK through `LoadLibrary`, so without
  `wbshsdk.dll` beside it the shell runs exactly as before.

Every release carries `wbsh-sdk-<version>.zip` with the header, a template
project for MSBuild and CMake, the samples and the SDK's symbols. See
[sdk/README.md](sdk/README.md) to write one.

---

## Building from source

Requires Windows 10 1903+, Visual Studio 2022 or Build Tools 2022 with the
**Desktop development with C++** workload, and optionally
[Inno Setup 6](https://jrsoftware.org/isdl.php) for the installer.

```powershell
& "$env:ProgramFiles\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" `
    .\wbsh\wbsh.vcxproj -p:Configuration=Release -p:Platform=x64

.\installer\build.ps1                        # installers, portable ZIPs and the SDK ZIP, version from version.props
```

The binary lands at `x64\Release\wbsh.exe`; installer output goes to
`installer\output\`. [CONTRIBUTING.md](./CONTRIBUTING.md) has the repository
layout, the architecture tour and the test suites: the golden suite, the
differential tests against GNU bash, bash's own suites, the real-world corpus
and the benchmark perf gate.

---

## Comparison

|                          | wbsh         | Git Bash      | MSYS2          | Cygwin       | WSL              |
|--------------------------|--------------|---------------|----------------|--------------|------------------|
| Native Windows binary    | yes          | yes (MSYS)    | yes (MSYS)     | yes          | no (Linux VM)    |
| Single-binary install    | **yes**      | no            | no             | no           | no               |
| Coreutils bundled        | **yes**      | yes (MSYS)    | yes (pacman)   | yes          | yes              |
| Modern console (VT mode) | **yes**      | MinTTY        | MinTTY         | MinTTY       | n/a              |
| `fork()` emulation       | spawn-only   | full (slow)   | full (slow)    | full (slow)  | n/a              |
| Real Linux syscalls      | no           | no            | no             | no           | yes              |
| Signals to programs      | wbsh only¹   | MSYS only     | MSYS only      | Cygwin only  | yes              |
| pty for child programs   | console²     | MSYS pty      | MSYS pty       | Cygwin pty   | yes              |
| Permission bits          | emulated³    | `noacl`       | `noacl`        | NTFS ACLs    | yes              |

¹ A wbsh can trap what another wbsh sends; any other program is terminated
with exit code 128 + n. ² `script` and `unbuffer` give a command a ConPTY.
³ Read-only attribute plus mode bits kept in an NTFS stream, read only by
wbsh and its commands.

wbsh trades full POSIX behavior for being small, fast to start, and clean to
integrate. If you need a real Linux environment, use WSL. What that trade
means in practice is listed in
[docs/posix-differences.md](docs/posix-differences.md); a sourced, dated
comparison with the other ways to get bash on Windows is in
[docs/comparison.md](docs/comparison.md).

---

## Limitations

What wbsh does not do, or does differently, today. The compatibility
numbers above are the authoritative list; this is the summary. The
process, signal, permission, file-system and terminal differences are
detailed in [docs/posix-differences.md](docs/posix-differences.md).

- **Not GNU bash.** wbsh is its own implementation. It passes 68 of bash 5.3's
  86 upstream suites byte for byte, and the other 18 differ only where
  Windows lacks a Unix path or file name, or where wbsh differs on purpose:
  `BASH_VERSION` 5.2, scripts run without an execute bit, `~/.wbsh_*`
  startup files instead of `~/.bash_logout`, its own history file format
  and its own `help` text. Every difference is listed in
  [docs/compat-ledger.md](docs/compat-ledger.md).
- **Frameworks.** oh-my-bash, bash-it, bash-completion, nvm, sdkman, asdf
  and git's completion and prompt scripts load, within one run of Git Bash
  over 1,034 files. The plugins that fail are almost all for tools that
  are not installed, and fail under Git Bash too. See the
  [corpus snapshot](docs/corpus/2026-11-24-local.md).
- **No `fork()`.** Subshells and command substitutions run in-process,
  pipeline stages of shell code in a child wbsh, and external programs
  start with `CreateProcess`. Scripts that depend on fork semantics
  (sharing file offsets with a forked child, `$BASHPID` of a forked copy of
  the shell) can behave differently.
- **Windows file system.** Case-insensitive names; no `/usr`, `/bin` or
  `/etc`; `/dev`, `/proc` and FIFOs are emulated and visible only to wbsh
  and its commands; file names cannot contain `: * ? " < > |`; symlinks
  need Developer Mode or administrator rights.
- **Permissions.** No uid/gid model and no setuid, setgid or sticky bits.
  `chmod` bits are kept in an NTFS stream that only wbsh and its commands
  read; NTFS ACLs decide who may open a file.
- **Signals.** Only another wbsh can trap a signal; any other program is
  terminated with exit code 128 + n (`STOP` and `CONT` suspend and resume
  it).
- **Bytes vs characters.** Strings are UTF-8; bytes that are not valid UTF-8
  (for example from `$'ÿ'`) are not always kept byte for byte.
- **Distribution.** Releases are not code-signed yet (prepared, see
  [docs/signing.md](docs/signing.md)); ARM64 builds are produced by CI but
  unproven on ARM64 hardware; the winget listing is pending.
- **Interactive mode only for the REPL.** `wbsh -i script.sh` and `wbsh -i -c`
  do not set `i` in `$-`.

---

## Roadmap

- Raise the upstream-suite and corpus numbers in the scoreboard; the
  failures in [docs/compat-ledger.md](docs/compat-ledger.md) are the work list.
- winget-pkgs and Scoop extras listings, active code signing (SignPath), and ARM64 builds proven on ARM64 hardware.

---

## Contributing

Pull requests welcome; see [CONTRIBUTING.md](./CONTRIBUTING.md) for the
architecture tour, conventions, test harness and PR checklist. In short: match
the existing style, one change per PR, add a test when changing executor
behavior, and have a clean Release build, `python tools/check_style.py` and the
golden suite green before opening it.

Bug reports should include the wbsh version, Windows build, and the smallest
reproducing script.

---

## License

[MIT](./LICENSE) © 2026 Tomas Trachta.
