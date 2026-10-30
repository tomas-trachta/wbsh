# wbsh

A Bash-compatible shell for Windows: a drop-in replacement for Git Bash,
written from scratch in C++17 with a real POSIX shell grammar, a readline-style
line editor, and bundled coreutils so a fresh install is useful immediately.

![wbsh running the test suite in Windows Terminal](./preview.png)

> Status: **early but released.** Tagged 1.0.x releases exist and a golden test
> suite runs on every build, but there is no CI yet and behavior may change
> between minor versions.

---

## Why wbsh

Git Bash is a thin re-skin of MSYS2, and its quirks leak through: PATH
translation surprises, slow `fork` emulation, an aging MinTTY. wbsh is built
for modern Windows instead:

- **Native console.** VT-mode output and immersive dark mode; renders correctly
  in Windows Terminal, conhost, VS Code, and JetBrains terminals.
- **One self-contained binary.** No MSYS or Cygwin layer. ~1 MB exe, ~2.5 MB
  installer including the VC++ runtime.
- **Real Bash semantics.** Lexer, parser, AST and expander, not a wrapper
  around `cmd.exe`.
- **Bundled coreutils.** `ls`, `grep`, `sed`, `awk`, `find`, `tar`, `curl`,
  hashes and more are built in. System `git`, `vim`, `less` are auto-discovered.
- **Path translation.** `/c/Users/...` ↔ `C:\Users\...` when invoking native
  Windows executables.

---

## Install

**Installer.** Download `wbsh-setup-x64.exe` from
[Releases](https://github.com/tomas-trachta/wbsh/releases). Per-user, no UAC,
installs to `%LOCALAPPDATA%\Programs\wbsh`, with opt-in tasks to add `wbsh` to
your `PATH` and an "Open wbsh here" Explorer context-menu entry.

**wbshterm.** Every release also ships `wbshterm-setup-x64.exe`, a terminal
that hosts wbsh on a pseudoconsole with themes, a config file, scrollback and
selection, tmux-style panes, and shell integration. It bundles `wbsh.exe`, so
it works on its own; installing both is fine. See
[wbshterm/README.md](wbshterm/README.md).

![wbshterm at startup: the screenfetch panel, Catppuccin Mocha theme](./preview_terminal.png)

**Portable ZIP.** `wbsh-<version>-portable-x64.zip` (or the `wbshterm-` one):
extract and run. No registry, no PATH changes.

**winget / scoop.** Not yet published.

---

## Quick start

```sh
$ wbsh
 wbsh 1.0.8 — a Bash-compatible shell for Windows
   type `exit` or press `Ctrl-D` to quit
trach@DESKTOP /c/Users/trach (main)$ ls
Documents  Downloads  Desktop  ...

$ wbsh -c 'for i in {1..3}; do printf "%02d\n" "$((i * 7))"; done'
07
14
21
```

Pipelines, redirection, control flow, functions, command substitution,
arithmetic, brace, parameter and glob expansion, here-docs, here-strings and
traps all work. Tilde expansion covers `~`, `~+`, `~-` and `~name`, which is
that user's profile directory (from the account's ProfileList entry, or
where Windows would put a first profile); a name that is no user account
stays literal, as in bash, and Tab completes `~na` to user names.

```
wbsh                          interactive shell (TTY auto-detect)
wbsh [opts] -c <command>      run / dump the given string
wbsh [opts] <file>            run / dump the file (- for stdin)

  -i, --interactive           force interactive REPL
  -r, --run                   execute the script (default for files is an AST dump)
  -e, --expand                dump expanded words
  -t, --tokens                dump the token stream
  --no-ast                    suppress the AST dump
  -h, --help                  show help
```

The default AST dump for files is a debugging aid; pass `-r` to run scripts.

---

## Line editing

Persistent history, kill ring, programmable and per-tool tab completion (git,
docker, npm, cargo, kubectl), reverse-incremental search, and inline
predictions: the rest of the best matching history entry appears as dim ghost
text, and entries whose last run failed are never suggested. Bindings follow
readline:

| Keys | Action |
|------|--------|
| `Tab`                                | complete; second `Tab` lists candidates |
| `→` at end of line                   | accept the inline prediction |
| `← / →`, `Ctrl-B / Ctrl-F`           | move by one character |
| `Home / End`, `Ctrl-A / Ctrl-E`      | start / end of line |
| `↑ / ↓`, `Ctrl-P / Ctrl-N`           | walk history |
| `Ctrl-R` / `Ctrl-S`                  | search history backward / forward |
| `Esc` / `Ctrl-G`                     | cancel the search |
| `Alt-B / Alt-F`, `Ctrl-← / Ctrl-→`   | move by one word |
| `Ctrl-U / Ctrl-K`                    | kill to start / end of line |
| `Ctrl-W`                             | kill the previous whitespace-separated word |
| `Alt-Backspace / Alt-D`, `Ctrl-Del`  | kill the previous / next word |
| `Ctrl-Y`, `Alt-Y`                    | yank the last kill, then cycle older kills |
| `Ctrl-T` / `Alt-T`                   | swap characters / words |
| `Alt-U / Alt-L / Alt-C`              | upcase / downcase / capitalize the word |
| `Ctrl-_`, `Ctrl-X Ctrl-U` / `Alt-R`  | undo / undo every change to the line |
| `Alt-.` / `Alt-_`                    | insert the last word of the previous command (repeat: older ones) |
| `Ctrl-X Ctrl-E`                      | edit the line in `$VISUAL` / `$EDITOR` (default notepad), then run it |
| `Ctrl-V`                             | paste from the clipboard |
| `Ctrl-L`                             | clear the screen |
| `Ctrl-C`                             | abandon the line |
| `Ctrl-D` (empty line)                | exit |

Completed file names are quoted the way the word was typed (`"My Doc` →
`"My Documents/`) or get backslashes (`My\ Documents/`), the value after `=`
completes on its own (`--file=./sr`), and names match regardless of case
(`set completion-ignore-case off` in `~/.inputrc` to change that).

`Esc` followed by a key works as Alt with it; AltGr characters type as usual.
Bindings can be changed with `bind` (`bind -p` lists them, `bind -l` the
commands, `bind -x` runs a shell command with `READLINE_LINE` /
`READLINE_POINT`) and in `~/.inputrc` (or `$INPUTRC`), read before `.wbshrc`,
with readline's syntax: `"\C-t": transpose-chars`, `Meta-q: upcase-word`,
`"\ez": "macro text"`, `set bell-style none`, `$if mode=emacs` ... `$endif`,
`$include`.

---

## Configuration

`~/.wbshrc` is sourced at the start of every interactive session:

```sh
export EDITOR=code
alias ll='ls -lah'
alias gs='git status'
PS1='\[\e[35;1m\]\w\[\e[0m\]\g \$ '
```

| Variable    | Effect                                                          |
|-------------|-----------------------------------------------------------------|
| `PS1`, `PS2` | Primary and continuation prompts. Escapes below.               |
| `PATH`      | Stored in POSIX form, auto-translated for spawns.               |
| `HOME`      | Defaults to `%USERPROFILE%` in POSIX form.                      |
| `HISTFILE`  | History file (default `$HOME/.wbsh_history`); each command is appended as it finishes. Empty: not saved. |
| `HISTSIZE`, `HISTFILESIZE` | Entries kept in memory / in the file (default 5000; negative: no limit). |
| `HISTCONTROL`, `HISTIGNORE` | `ignorespace`, `ignoredups`, `ignoreboth`, `erasedups`; colon-separated patterns (`&` = previous entry) left out of history. |
| `COLUMNS`, `LINES` | Updated on resize; fires the `WINCH` trap.               |
| `WBSH_GIT_NO_DIRTY` | Skip the working-tree check in the `\g` prompt (huge repos). |
| `WBSH_GIT_STATUS_WAIT` | Milliseconds the prompt waits for `\g`'s background `git status` (default 50) before showing the last known state and redrawing when the new one arrives. |
| `WBSH_NO_PATHCONV`, `MSYS_NO_PATHCONV` | Suppress POSIX→Win32 argument translation for spawned commands. |
| `WBSH_PAGER` | `builtin` makes `less` use the bundled pager even when a `less` is on PATH. |

Prompt escapes: `\u` user, `\h` / `\H` short / long hostname, `\w` / `\W`
working directory / its basename, `\g` ` (branch)` inside a git repo, `\t`
time, `\s` `wbsh`, plus `\n` `\r` `\a` `\e` `\\` `\$` and the `\[` `\]`
non-printing markers. Default `PS1`:

```
\[\e[32;1m\]\u@\h\[\e[0m\] \[\e[36;1m\]\w\[\e[0m\]\g\$
```

---

## Bundled builtins

**Shell:** `:`, `true`, `false`, `echo`, `printf`, `exec`, `pwd`, `cd`, `exit`,
`return`, `break`, `continue`, `export`, `unset`, `shift`, `set`, `eval`,
`source` / `.`, `type`, `command`, `read`, `test` / `[`, `local`, `alias`,
`unalias`, `history`, `trap`, `getopts`, `declare` / `typeset`, `mapfile` /
`readarray`, `shopt`, `let`, `umask`, `hash`, `times`, `caller`, `help`,
`compgen`, `complete`, `compopt`, `readonly`, `jobs`, `wait`, `fg`, `bg`,
`disown`.

`exec PROGRAM` cannot replace the shell, since Windows has no `execve`: the
program runs as a child the shell waits for and then ends with, passing on
the program's own exit code (an NTSTATUS or a code past 255 included). It
runs in a job that ends with the shell, so killing the shell kills the
program too, while whatever the program leaves running when it exits on its
own carries on. Ctrl+C and Ctrl+Break are left to the program. Its PID is
not `$$`, so a script that writes `$$` to a pid file and then `exec`s a
daemon records the wrapper's PID; use `$!` of a background start or the
program's own pid file instead.

Windows delivers no POSIX signals, so every wbsh keeps a mailbox named after
its PID, and `kill` (`-NAME`, `-SIGNAME` in any case, `-s`, `-n`, `-NUM`,
`-l`), `pkill`, `killall` and `timeout` post catchable signals there. The
shell takes them between commands, in `wait` (which returns 128 + n, as in
bash) and in `sleep` (which runs the trap at once and sleeps on), and then
runs the trap, ignores the signal (`trap ''`; CHLD, CONT, URG and WINCH by
default, and INT, QUIT and TERM in an interactive shell), or ends with
128 + n: the EXIT trap still runs, and the command the shell was waiting for
ends with it. A shell busy in a long builtin (`tail -f`, `crond`, `httpd`)
that gets a signal it neither traps nor ignores ends half a second later, as
an uncaught signal would end it. A trapped signal that comes while an
external command runs is handled once the command is done, as in bash. KILL,
STOP / TSTP and CONT work on any process as before, and a process that is no
wbsh, or a wbsh in another logon session, is terminated with 128 + n.

**Files:** `ls`, `cat`, `cp`, `mv`, `rm`, `mkdir`, `mkfifo`, `mknod`, `rmdir`, `touch`, `ln`,
`link`, `unlink`, `stat`, `chmod`, `chown`, `chgrp`, `chattr`, `lsattr`,
`install`, `file`, `du`, `df`, `realpath`, `readlink`, `basename`, `dirname`,
`find`, `dd`, `split`, `truncate`, `shred`, `sync`, `fsync`, `flock`, `mktemp`,
`pushd`, `popd`, `dirs`.

**Text:** `head`, `tail` (`-f`), `wc`, `sort`, `uniq`, `tr`, `cut`, `tee`,
`paste`, `join`, `tac`, `rev`, `nl`, `fold`, `column`, `expand`, `unexpand`,
`comm`, `shuf`, `tsort`, `strings`, `grep` / `egrep` / `fgrep`, `sed`,
`awk` / `gawk`, `diff` (`-r`), `cmp`, `patch`, `dos2unix`, `unix2dos`,
`iconv`, `xargs`, `less`, `more`, `vi` / `view`, `ed`.

**Encoding and hashing:** `base64`, `base32`, `uuencode`, `uudecode`, `xxd`,
`od`, `hexdump` (`-e` / `-f` format strings), `hd`, `md5sum`, `sha1sum`,
`sha256sum`, `sha384sum`, `sha512sum`, `sha3sum` (all with `-c`), `cksum`,
`sum`, `crc32`.

**Archives and compression:** `tar` (gzip, bzip2, xz, lzma, lzop and compress
are detected on read; `-z` / `-j` / `-J` / `--lzma` / `--lzop` / `-a` on
write), `gzip`, `gunzip`, `zcat`, `bzip2`, `bunzip2`, `bzcat`, `xz` / `unxz` /
`xzcat`, `lzma` / `unlzma` / `lzcat` (`-0`…`-9`, `-e`, `-C` check type,
`-F xz|lzma`), `lzop` / `unlzop` / `lzopcat`, `uncompress`, `zip`, `unzip`,
`cpio` (newc, crc, odc, bin, ustar and tar, both ways), `ar` (create, replace,
append, delete, move; GNU long names, deterministic headers unless `U`).
`lzop` keeps its input unless given `-U`, as lzop does.

**Processes and system:** `ps`, `pgrep`, `pkill`, `pidof`, `kill`, `killall`,
`top`, `nice`, `renice`, `nohup`, `setsid`, `timeout`, `watch`, `script`,
`unbuffer`, `crontab`,
`crond`, `uptime`, `free`, `nproc`, `env` (`-i`, `-u`, `NAME=value command`),
`printenv`, `whoami`, `logname`, `groups`, `id`, `su`, `sudo`, `hostname`,
`uname`, `date`, `cal`, `sleep`, `usleep`, `stty`, `reset`, `tput`, `ttysize`,
`clear`, `locale`.

**Network:** `curl`, `wget`, `nc` / `netcat`, `ftp`, `ftpget`, `ftpput`,
`whois`, `httpd`.

**Other:** `echo`, `printf` (`%b`, `%q`, `-v`), `seq`, `expr`, `bc`, `dc`,
`factor`, `getopt`, `uuidgen`, `ascii`, `ipcalc`, `ts`, `yes`, `which`,
`make` / `gmake`, `man`, `apropos`, `whatis`, `fzf`, `tmux`, `utils`.

`/dev/zero`, `/dev/random`, `/dev/urandom` and `/dev/full` exist for
redirections and for every bundled command that reads a file.

A read-only `/proc` exists the same way, for redirections, globs, `test`,
`ls`, `readlink` / `realpath` and the bundled commands (external programs do
not see it): `cpuinfo` (one block per logical processor, CPUID flags under
Linux's names), `meminfo`, `uptime`, `loadavg` (Windows keeps no load average,
so it reads 0.00), `version` (wbsh's own, so WSL checks stay false), `self`,
and `/proc/<pid>/` with `cmdline`, `comm`, `status` and `exe`; the shell's own
process adds `cwd`, `environ` and `fd/<n>`. Files read as size 0, as on Linux,
and their text is made when they are opened. A pipeline stage or `$(...)`
runs in a child wbsh, so `/proc/self` there is that child, and it sees only
the three standard descriptors.

`mkfifo` and `mknod NAME p` make FIFOs. Windows keeps named pipes outside the
file system, so a FIFO is a small marker file that wbsh joins through a
named pipe: redirections and the bundled commands treat it as a FIFO (an
open for reading waits for a writer and the reverse, the reader sees end of
file when its writer closes, Ctrl+C ends the wait), and `[ -p ]`, `ls` (type
`p`, `-F` appends `|`), `stat` ("fifo") and `find -type p` know it. Each
connection joins one reader and one writer; a second writer waits for the
next reader. Opened with `<>` (`exec 3<>fifo`), it opens at once and takes
writer after writer without end of file, as on Linux, though the descriptor
reads only. Programs outside wbsh see the marker file, not a FIFO.
AF_UNIX socket files, the reparse points Windows 10 1803 and later create
for them (Docker Desktop, ssh-agent ports, language servers and WSL interop
make them), are sockets to `[ -S ]` (through links, as in bash), `ls` (type
`s`, `-F` appends `=`), `stat` ("socket") and `find -type s`, so
`until [ -S "$sock" ]; do sleep 0.1; done` waits for one. `-u`, `-g`, `-k` and
`-b` stay false: Windows has no setuid, setgid or sticky bits, nor block devices.

`less` hands over to a real `less` when one is on PATH and stdout is a
console; otherwise (or with `WBSH_PAGER=builtin`) the built-in pager runs.

`tmux` is not the real tmux: under wbshterm it opens pane mode (splits, status
bar, `Ctrl-B` prefix; see [wbshterm/README.md](wbshterm/README.md#panes)),
anywhere else it says so and exits non-zero. There is no server and nothing
outlives the window.

`fzf` is a fuzzy picker in the spirit of
[junegunn/fzf](https://github.com/junegunn/fzf): pipe lines in, type to narrow,
Enter prints the selection. With no input it lists the directory tree. It works
mid-pipeline because it talks to the console device directly. Typed bare at the
prompt it acts on the pick: a directory is entered, a file opens through its
file association. Captured or piped uses keep printing the selection.

`vi` / `view` is a busybox-style full-screen editor for the console, Windows
Terminal and wbshterm: counts, motions, operators, `.` repeat, multi-level
undo / redo, marks, named registers, searches, scrolling, autoindent and ex
commands (`:s`, `:g`, `:r !cmd`, `:range!filter`, `:set`, several files). It
handles UTF-8 (wide characters take two cells), tabs, long lines and resizing,
and keeps CRLF line endings. Visual mode, text objects, macros and `:map` are
not there; use a real vim from PATH for those. When stdin is not a console it
reads keys from stdin and draws nothing, so it can be scripted:
`printf 'ihello\033:wq\n' | vi file`. `ed` is the POSIX line editor with GNU's
extras (`W`, `wq`, `z`, `E`, `-p` / `-s` / `-v` / `-l`): full addressing, `s`
with back-references and multi-line replacements, `g` / `v` / `G` / `V`, undo,
marks and `r !` / `w !` / `e !`.

`make` / `gmake` is GNU make 4.4 compatible: every assignment flavour,
`define`, conditionals, `include`, remaking of makefiles (main and included)
with a restart, the GNU functions (`eval`, `call`, `shell`, `file`, `let`,
`intcmp`, ...), pattern, static-pattern, double-colon, grouped and suffix
rules, GNU's built-in C / C++ rules, VPATH / vpath, target- and
pattern-specific variables, `.SECONDEXPANSION`, `.ONESHELL`,
`.DELETE_ON_ERROR` and the other special targets, and `-n -s -k -i -q -t -B -e
-r -R -p -C -f -I -o -W -w`, with GNU's messages. Each recipe line runs in a
fresh wbsh: `SHELL = /bin/sh` or `bash` means wbsh, and `.SHELLFLAGS` such as
`-eu -o pipefail` reach it as `set` options; `SHELL = cmd` (or another Windows
shell) runs recipes there. `$(MAKE) -C sub` runs the same make again with
MAKEFLAGS and MAKELEVEL passed down. `-j` is accepted, but jobs run one at a
time, and archive members (`lib(member)`) are not supported.

`nice`, `renice`, `ps` and `top` share one mapping between niceness and Windows
priority classes: -20..-13 HIGH, -12..-5 ABOVE_NORMAL, -4..3 NORMAL, 4..11
BELOW_NORMAL, 12..19 IDLE (read back as -16, -8, 0, 8, 16; REALTIME is never
set). `nohup` and `setsid` start the command on a hidden console of its own,
so Ctrl+C, Ctrl+Break and closing the window leave it running; `setsid` keeps a
command whose standard streams are still the console on it. Ctrl+C while
`nohup` or `setsid -w` waits only gives the prompt back. `top` is procps-style
(`-b -n -d -p -u -o -c -i`; keys `P M N T R c i k r d h q`). wbsh cannot switch
users: `sudo` hands the command to the first `sudo` on PATH (Windows 11 24H2's
own, or gsudo) and otherwise explains how to get one; `su` points to
`runas /user:NAME wbsh`.

`script` (util-linux's: `-c`, `-a`, `-e`, `-f`, `-q`, `-I` / `-O` / `-B` log
files) and `unbuffer` (expect's, with `-p`) run a command on a pseudoconsole
(ConPTY) of its own, so it sees a console on stdin, stdout and stderr however
wbsh's own streams are redirected, and acts as it would interactively:
`unbuffer cmd | less` and `script -qc cmd /dev/null > out` keep its colours and
line-by-line output. The output comes back as the console draws it, with CR LF
line ends as from a Unix pty; the session follows the console's size, and
`stty` inside it changes the pseudoconsole that every program in it shares.
Keys typed at a console go through untranslated (Ctrl+C included); piped input
is sent as typed lines, ending with Ctrl+Z, Enter (a console's end of file)
and Ctrl+D. `script` runs wbsh, not `$SHELL`, which on Windows usually names
another environment's shell. Both use the `conpty.dll` beside `wbsh.exe` when
there is one (Microsoft's current ConPTY, as wbshterm does) and Windows' own
otherwise. Programs built on the Cygwin / MSYS runtime still see a Windows
console, not their emulated pty.

`crontab` (cronie's: `FILE` / `-`, `-l`, `-e` with `$VISUAL` / `$EDITOR` /
`vi`, `-r [-i]`, `-T FILE`) and `crond` (busybox's: `-f`, `-b`, `-l N`,
`-d N`, `-L FILE`, `-c DIR`) are cron for one Windows account. Crontabs live
in `%APPDATA%\wbsh\crontabs\<user>` (`-c DIR` or `$WBSH_CRONTAB_DIR` moves
them) and take lists, ranges, steps, names, `@reboot` … `@yearly`,
`NAME=value` lines and `%`; daylight-saving changes are handled as cronie
does. `crond` starts a hidden copy of itself and returns; jobs run through
wbsh without a window, in `$HOME`, and since there is no mail their output is
appended to `DIR/.crond.log` (`MAILTO=""` discards it, `MAILTO=/some/file`
sends it there). One crond runs per directory; stop it with
`kill $(cat "$APPDATA/wbsh/crontabs/.crond.pid")` (`pkill crond` finds
nothing, the process is `wbsh.exe`). wbsh registers nothing with Task
Scheduler; to start crond at logon, put a shortcut to `wbsh.exe -r -c crond`
in `shell:startup`, or run
`schtasks /Create /SC ONLOGON /TN wbsh-crond /TR "\"C:\path\to\wbsh.exe\" -r -c crond"`
from cmd.

`chown` / `chgrp` (`-R -h -H -L -P -v -c -f --from --reference`) set the NTFS
owner and group. Asking for the current owner always succeeds; giving a file
to another account needs an elevated shell and otherwise fails with
"Operation not permitted", as on Linux. `install` follows GNU (`-d -D -t -T -m
-o -g -p -C -b / -S -v -s`); a mode maps onto the read-only attribute as in
`chmod`, and `-s` runs `strip` only when one is on PATH. `file` identifies
text encodings, scripts, executables (PE / ELF / Mach-O), archives,
compressed streams (`-z` looks inside), images, audio and documents in
file(1)'s words (`-b`, `-i`, `--mime-type`, `-L`, `-f`). `man`, `apropos` and
`whatis` show a page for every bundled command and builtin, page through
`$MANPAGER` / `$PAGER` / less, and hand other names to an external `man` if
there is one. `locale` reports LANG / LC_* like glibc, falling back to the
Windows user locale (e.g. `cs_CZ.UTF-8`), and answers `-a`, `-m`, `-k` and
keyword queries from Windows' locale data.

`nc` / `netcat` connects or listens over TCP and UDP (`-l`, `-k`, `-u`, `-w`,
`-q`, `-N`, `-z` port scans with ranges, `-e` / `-c` to attach a program).
`whois` speaks RFC 3912, following IANA's referral and the thin-registry
"Registrar WHOIS Server" for .com / .net. `ftp` is the classic interactive
client (`~/.netrc`, passive EPSV / PASV by default, active PORT / EPRT via
`passive`, `ftp ftp://user:pass@host/path` autofetch) with busybox's `ftpget` /
`ftpput` beside it. `httpd` is a busybox-style static server (`-p [IP:]PORT`,
`-h DIR`, `-f`, `-v`; GET / HEAD, ranges, If-Modified-Since, directory indexes,
keep-alive, path-traversal protection) that also runs CGI scripts under
`/cgi-bin/` (`.exe` / `.com` / `.bat` / `.cmd` directly, `.sh` / `.cgi` or `#!`
scripts through wbsh, anything else refused; a 30 s limit per request) and reads a
busybox `httpd.conf` (`A:` / `D:` allow and deny, `/path:user:pass` basic auth
with plain-text passwords, `.ext:type`, `Ennn:/page`, `I:name`). Request paths
are decoded and normalised before the auth and CGI checks, and auth rules match
case-insensitively, as NTFS names do. Listeners bind
every interface unless given an address, so the first one may raise the
Windows Firewall prompt; `-s 127.0.0.1` / `-p 127.0.0.1:8080` avoids it.

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

A util's command redirects, pipes and sets `$?` like any other, has `help`
text and Tab completion of its own, and cannot take a bundled command's name.
The contract is C (`sdk/include/wbshsdk.h`), ABI-versioned and unlinked: the
host hands a util a table of functions when it loads, so any compiler that
makes a DLL will do. A mismatched or non-util DLL is named and skipped without
affecting the others, and `utils` says why. `utils load <dll>` and
`utils reload` keep the edit-run loop inside one shell. `wbsh.exe` reaches the
SDK through `LoadLibrary`, so without `wbshsdk.dll` beside it the shell runs
exactly as before.

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
`installer\output\`.

### Layout

```
wbsh/        The shell: wbsh.vcxproj, src/, tests/ (golden suite, run-all.sh)
wbshterm/    The terminal: Win32 window, renderer, VT parser, panes; vendored ConPTY
sdk/         The util SDK: wbshsdk.dll, include/wbshsdk.h, template/, samples/, tests/
installer/   Inno Setup script and build.ps1
tools/       check_style.py (runs before every build), make_icon.py
docs/        Doxygen config; regenerate with `doxygen docs/Doxyfile`
```

[CONTRIBUTING.md](./CONTRIBUTING.md) has the file-by-file architecture tour.

---

## Testing

```sh
# From wbsh/tests/, after building:
../../x64/Release/wbsh.exe -r run-all.sh                  # every script exits 0
WBSH_GOLDEN=1 ../../x64/Release/wbsh.exe -r run-all.sh    # also diff against expected/
```

Each `wbsh/tests/*.sh` script exercises one slice of behavior and runs inside
wbsh itself, so the suite is an end-to-end check of the whole pipeline. The SDK
has its own checks in `sdk\tests\sdk.ps1`, which need the samples built into
`x64/Release/plugins`.

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

wbsh trades full POSIX behavior for being small, fast to start, and clean to
integrate. If you need a real Linux environment, use WSL.

---

## Roadmap

- Real job control: `Ctrl-Z` and stopped jobs.
- CI running the golden suite on every push.
- winget / scoop publication and code-signed releases.

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
