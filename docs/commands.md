# Command notes

How wbsh's bundled commands behave, and where Windows makes them differ from
Linux. The list of commands is in the [README](../README.md#commands).

- [Shell behaviour on Windows](#shell-behaviour-on-windows)
- [Git Bash helpers](#git-bash-helpers)
- [Files](#files)
- [Text and editors](#text-and-editors)
- [Encoding and hashing](#encoding-and-hashing)
- [Archives and compression](#archives-and-compression)
- [Processes and system](#processes-and-system)
- [Network](#network)
- [Build](#build)
- [Other](#other)

---

## Shell behaviour on Windows

### Tilde expansion

Tilde expansion covers `~`, `~+`, `~-` and `~name`.

- `~name` is that user's profile directory, taken from the account's
  ProfileList entry, or where Windows would put a first profile.
- A name that is no user account stays literal, as in bash.
- Tab completes `~na` to user names.

### Identity: uname, OSTYPE, UID

wbsh answers the way Git Bash does: `uname -s` is `MINGW64_NT-<version>`,
`OSTYPE` is `msys`, `MACHTYPE` ends in `-pc-msys` and `MSYSTEM` is `MINGW64`
when the environment has none. Scripts that check
`case $(uname -s) in MINGW*|MSYS*)` take their Windows branch. Launchers such
as `gradlew` and the npm shims call `cygpath` on that branch, and `cygpath` is
built in (see [Git Bash helpers](#git-bash-helpers)). `WBSH_UNAME_S=wbsh`
brings back the old answer for a script that needs the generic POSIX branch.

There is no uid model. `UID` and `EUID` are readonly and hold the id that
`id -u` prints (1000), never 0, also in an elevated shell. `HOSTNAME` is the
computer name, the same as `hostname`. `SHLVL` goes up by one in each new wbsh
process; subshells and pipeline stages keep the level of their shell.

### Command lookup

A command found on PATH is looked up in the Windows order: `NAME.exe`,
`NAME.cmd`, `NAME.bat`, and then `NAME`. Git Bash takes `NAME` first. So when a
folder holds both a POSIX launcher and a batch file, such as Maven's `bin/mvn`
and `bin/mvn.cmd`, wbsh runs the batch file. Many such launchers walk up with
`cd ..` until `pwd` prints `/`. In wbsh, `/` is the root of the current drive
and `pwd` prints `/c/` there, so these loops do not end.

A command named by its path (`./gradlew`, `"$HOME/bin/tool"`) runs that file,
as in bash, when it starts with `#!` or is a Windows program. Without such a
file, the path gets the same extensions as a PATH lookup.

A file test on a missing name also finds the name with `.exe` added, as in Git
Bash: `[ -x "$JAVA_HOME/bin/java" ]` is true when `java.exe` exists.

### Arguments for native programs

A program that is not built on MSYS or Cygwin gets its POSIX path arguments in
Win32 form, by the rules Git Bash's MSYS runtime uses:

| Argument | Passed as |
| --- | --- |
| `/c/Users`, `/tmp/x`, `/dev/null` | `C:\Users`, `%TEMP%\x`, `nul` |
| `--opt=/c/x`, `VAR=/c/x`, `-I/c/x`, `-Wl,-rpath,/c/x` | the path part converted |
| `/c/x:/d/y`, `-I/c/x:/d/y` | `C:\x;D:\y` |
| `//c`, `//F`, `//PID` | `/c`, `/F`, `/PID`, for `cmd //c`, `taskkill //F` |
| `//server/share` | unchanged (UNC) |
| URLs, `s/a/b/`, `a:b`, `file.cpp:12`, `./rel`, `~/x` | unchanged |
| anything with `*`, `?`, `[`, `]`, quotes or `;` | unchanged |

Where wbsh differs from Git Bash:

- A two-character `/x` stays as it is, so `cmd /c`, `where /q` and other
  Windows switches work without the `//` escape.
- wbsh has no MSYS root, so a path no mount covers (`/`, `/usr/bin`, `/opt/x`)
  goes out unchanged instead of under Git's install directory. A `:` list with
  such an entry stays whole, so `docker run -v /c/src:/app` keeps its meaning.
- Converted paths use backslashes.
- `/dev/stdin`, `/dev/stdout`, `/dev/stderr` and `/dev/tty` stay as they are.
- Environment variables other than `PATH` and `HOME` are not converted, so
  `MSYS2_ENV_CONV_EXCL` has nothing to do.

Turning it off:

- `MSYS_NO_PATHCONV` or `WBSH_NO_PATHCONV`, set and not empty, turns it off
  for every argument.
- `MSYS2_ARG_CONV_EXCL` is `*` for every argument, or a `;`-separated list of
  prefixes: `MSYS2_ARG_CONV_EXCL='--prefix=;/c/keep'` leaves the arguments
  starting with either alone.
- Programs from an MSYS2 or Cygwin install, or from Git's `usr\bin`,
  `mingw32\bin` and `mingw64\bin`, get their arguments unchanged.

### exec

`exec PROGRAM` cannot replace the shell, since Windows has no `execve`. The
program runs as a child that the shell waits for and then ends with.

- The shell passes on the program's own exit code, an NTSTATUS or a code past
  255 included.
- The program runs in a job that ends with the shell, so killing the shell
  kills the program too. Whatever the program leaves running when it exits on
  its own carries on.
- Ctrl+C and Ctrl+Break are left to the program.
- Its PID is not `$$`, so a script that writes `$$` to a pid file and then
  `exec`s a daemon records the wrapper's PID. Use `$!` of a background start or
  the program's own pid file instead.

### Signals

Windows delivers no POSIX signals, so every wbsh keeps a mailbox named after
its PID. `kill`, `pkill`, `killall` and `timeout` post catchable signals there.
`kill` takes `-NAME`, `-SIGNAME` in any case, `-s`, `-n`, `-NUM` and `-l`.

The shell takes a signal at one of these points:

- between commands;
- in `wait`, which returns 128 + n, as in bash;
- in `sleep`, which runs the trap at once and sleeps on.

It then does one of three things:

- runs the trap;
- ignores the signal, for `trap ''`, for CHLD, CONT, URG and WINCH by default,
  and for INT, QUIT and TERM in an interactive shell;
- ends with 128 + n. The EXIT trap still runs, and the command the shell was
  waiting for ends with it.

Some cases work differently:

- A shell busy in a long builtin (`tail -f`, `crond`, `httpd`) that gets a
  signal it neither traps nor ignores ends half a second later, as an uncaught
  signal would end it.
- A trapped signal that comes while an external command runs is handled once
  the command is done, as in bash.
- KILL, STOP / TSTP and CONT work on any process.
- A process that is no wbsh, or a wbsh in another logon session, is terminated
  with 128 + n.

### time

The `time` keyword reports elapsed, user and system time, as bash does.

- User and system time are the shell's own, for builtins and functions, plus
  that of every child process it waited for. A child's own children count only
  when it waited for them itself, as with `getrusage`.
- `TIMEFORMAT` takes bash's `%[p][l]R`, `%[p][l]U`, `%[p][l]S`, `%P` and `%%`.
  Set but empty, it turns the report off.
- `time -p` prints the POSIX format and ignores `TIMEFORMAT`.
- `time` alone times a null command; in POSIX mode it reports the user and
  system time of the shell and its children since the shell started.

### POSIX mode

`set -o posix`, `--posix`, a copy named `sh.exe`, or `POSIXLY_CORRECT` in the
environment turn on these parts of bash's POSIX mode:

- `POSIXLY_CORRECT` is set (unsetting it leaves POSIX mode), and
  `expand_aliases`, `inherit_errexit`, `shift_verbose` and
  `interactive_comments` go on; `set +o posix` puts them back.
- Special builtins (`.`, `:`, `break`, `continue`, `eval`, `exec`, `exit`,
  `export`, `readonly`, `return`, `set`, `shift`, `times`, `trap`, `unset`)
  are found before functions, and a function cannot take their name.
- Assignments before a special builtin stay in the shell (exported).
- In a non-interactive shell these end the shell: an error from a special
  builtin (a bad option, an invalid identifier, `shift` out of range, a
  syntax error in `eval` or a sourced file, `.` not finding its file), an
  expansion error such as `${x:?}` or a bad `$(( ))`, and an assignment
  error without a command or before a special builtin. `command` takes the
  special property away, as in bash.
- A failed prefix assignment cancels the command (status 1).
- `.` and `source` do not fall back to the current directory after `PATH`.
- Redirection targets are neither split nor globbed in a non-interactive
  shell.
- `time` alone reports the shell's times (above).

Not implemented: the `kill -l` and `trap` output formats, `echo` without
options under `xpg_echo`, the `cd` and `pwd` differences, and the job-control
message formats.

### /dev

`/dev/zero`, `/dev/random`, `/dev/urandom` and `/dev/full` exist for
redirections and for every bundled command that reads a file.

### /proc

A read-only `/proc` exists the same way, for redirections, globs, `test`, `ls`,
`readlink` / `realpath` and the bundled commands. External programs do not see
it.

| Path | Contents |
| --- | --- |
| `cpuinfo` | One block per logical processor, with CPUID flags under Linux's names |
| `meminfo`, `uptime` | As on Linux |
| `loadavg` | Reads 0.00, since Windows keeps no load average |
| `version` | `uname -s` and wbsh's own version, never `Microsoft`, so WSL checks stay false |
| `self` | The current process |
| `<pid>/` | `cmdline`, `comm`, `status`, `exe` |
| `<pid>/` of the shell itself | Also `cwd`, `environ`, `fd/<n>` |

- Files read as size 0, as on Linux, and their text is made when they are
  opened.
- A pipeline stage or `$(...)` runs in a child wbsh, so `/proc/self` there is
  that child, and it sees only the three standard descriptors.

### FIFOs

`mkfifo` and `mknod NAME p` make FIFOs. Windows keeps named pipes outside the
file system, so a FIFO is a small marker file that wbsh joins through a named
pipe.

- Redirections and the bundled commands treat it as a FIFO. An open for reading
  waits for a writer and the reverse, the reader sees end of file when its
  writer closes, and Ctrl+C ends the wait.
- `[ -p ]`, `ls` (type `p`, `-F` appends `|`), `stat` ("fifo") and
  `find -type p` know it.
- Each connection joins one reader and one writer. A second writer waits for
  the next reader.
- Opened with `<>` (`exec 3<>fifo`), it opens at once and takes writer after
  writer without end of file, as on Linux, though the descriptor reads only.
- Programs outside wbsh see the marker file, not a FIFO.

### Sockets and other file types

AF_UNIX socket files are the reparse points Windows 10 1803 and later create
for them. Docker Desktop, ssh-agent ports, language servers and WSL interop
make them. They are sockets to `[ -S ]` (through links, as in bash), `ls` (type
`s`, `-F` appends `=`), `stat` ("socket") and `find -type s`, so this waits for
one:

```sh
until [ -S "$sock" ]; do sleep 0.1; done
```

`-u`, `-g`, `-k` and `-b` stay false: Windows has no setuid, setgid or sticky
bits, nor block devices.

---

## Git Bash helpers

Scripts written for Git Bash call these, so wbsh has them built in.

**`cygpath`** converts paths with wbsh's own mounts, the ones every command
uses: `/c/x` is `C:\x` and `/tmp` is `%TEMP%`. Git Bash maps `/` to its install
directory, wbsh to the root of the current drive. Otherwise the output matches
Git's `cygpath`:

- `-u`, `-w`, `-m`, `-d` and `-t TYPE` pick the output form. `-a` makes a name
  absolute, `-p` converts a PATH list, and `-s` / `-l` give the short or long
  form.
- `-f FILE` reads names from FILE, or from stdin for `-`. `-0` / `-z` / `--null`
  ends each name with NUL.
- `-D`, `-H`, `-O`, `-P`, `-S`, `-W`, `-A` and `-F ID` print special folders.
- Windows names are normalised (`C:\x\..\y` is `/c/y`). POSIX names are printed
  as given, even with `-a`.

**`start`** opens a file, folder or URL with its associated program, or starts
a program with arguments without waiting, through `ShellExecuteEx`. `/wait` (or
`-w`) waits and returns the exit status, and `/min` / `/max` set the window
size. Arguments that are POSIX paths go out in Win32 form.

**`winpty`** runs its command as given. wbsh starts programs on a real console
or pseudo console already, so there is nothing left for winpty to do. Builtins,
functions and PATH programs all run, with their exit status. winpty's own
options (`-Xallow-non-tty`, `-Xplain`, `--mouse`) are ignored.

---

## Files

**`cat`** has GNU's `-A`, `-b`, `-e`, `-E`, `-n`, `-s`, `-t`, `-T`, `-u` and `-v`
and their long forms. `-v` writes `^X` and `M-` notation as GNU cat 8.32 does.
That is the version Git ships, so with `-E` a CR before a newline stays a CR.

**`chown` / `chgrp`** (`-R -h -H -L -P -v -c -f --from --reference`) set the
NTFS owner and group. Asking for the current owner always succeeds. Giving a
file to another account needs an elevated shell and otherwise fails with
"Operation not permitted", as on Linux.

**`chmod`** takes octal and symbolic modes (`u+x`, `go-w`, `a=rX`, `g=u`,
comma lists; a clause without `u`/`g`/`o`/`a` honours the umask) and
`-R -v -c -f --reference`. The owner write bit is the read-only attribute.
Execute is on by default for directories, `.exe .com .bat .cmd .ps1 .sh` files
and files starting with `#!` or `MZ`; any other mode chmod sets is kept in the
file's `wbsh.mode` NTFS stream (which copies and renames carry along). `test
-x`, `[[ -x ]]`, `ls -l`, `stat`, `find -perm` and `tar` all read the same
bits.

**`install`** follows GNU (`-d -D -t -T -m -o -g -p -C -b / -S -v -s`). A mode
maps onto the read-only attribute as in `chmod`, and `-s` runs `strip` only
when one is on PATH.

**`file`** identifies text encodings, scripts, executables (PE / ELF / Mach-O),
archives, compressed streams (`-z` looks inside), images, audio and documents
in file(1)'s words (`-b`, `-i`, `--mime-type`, `-L`, `-f`).

---

## Text and editors

`tail` has `-f`, and `diff` has `-r`.

### less

`less` hands over to a real `less` when one is on PATH and stdout is a console.
Otherwise, or with `WBSH_PAGER=builtin`, the built-in pager runs.

### vi

`vi` / `view` is a busybox-style full-screen editor for the console, Windows
Terminal and wbshterm.

- **Has:** counts, motions, operators, `.` repeat, multi-level undo / redo,
  marks, named registers, searches, scrolling, autoindent and ex commands
  (`:s`, `:g`, `:r !cmd`, `:range!filter`, `:set`, several files).
- **Handles:** UTF-8 (wide characters take two cells), tabs, long lines and
  resizing. It keeps CRLF line endings.
- **Lacks:** visual mode, text objects, macros and `:map`. Use a real vim from
  PATH for those.

When stdin is not a console it reads keys from stdin and draws nothing, so it
can be scripted:

```sh
printf 'ihello\033:wq\n' | vi file
```

### ed

`ed` is the POSIX line editor with GNU's extras (`W`, `wq`, `z`, `E`, `-p` /
`-s` / `-v` / `-l`): full addressing, `s` with back-references and multi-line
replacements, `g` / `v` / `G` / `V`, undo, marks and `r !` / `w !` / `e !`.

---

## Encoding and hashing

`hexdump` takes `-e` / `-f` format strings. `md5sum`, `sha1sum`, `sha256sum`,
`sha384sum`, `sha512sum` and `sha3sum` all take `-c`.

---

## Archives and compression

- **`tar`** detects gzip, bzip2, xz, lzma, lzop and compress on read, and
  writes them with `-z` / `-j` / `-J` / `--lzma` / `--lzop` / `-a`.
- **`xz` / `unxz` / `xzcat`, `lzma` / `unlzma` / `lzcat`** take `-0`…`-9`, `-e`,
  `-C` check type and `-F xz|lzma`.
- **`lzop`** keeps its input unless given `-U`, as lzop does.
- **`cpio`** handles newc, crc, odc, bin, ustar and tar, both ways.
- **`ar`** can create, replace, append, delete and move, with GNU long names,
  and writes deterministic headers unless given `U`.
- Extraction stays inside the target directory. `unzip` strips a leading `/`,
  drive or UNC prefix with a warning and skips names that climb out through
  `..` (exit status 1 either way, as Info-ZIP); `tar` and `cpio` refuse `..`
  names, and create symbolic links whose target is absolute or contains `..`
  only after every other member, so nothing is written through them.

---

## Processes and system

`env` takes `-i`, `-u` and `NAME=value command`.

**`uname`** answers as Git Bash does, so `case $(uname -s) in MINGW*|MSYS*)`
finds Windows:

- `-s` is `MINGW64_NT-10.0-<build>`, its prefix taken from `MSYSTEM` like
  MSYS2's: `MSYS_NT` for `MSYS`, `MINGW32_NT` for a 32-bit environment.
  `WBSH_UNAME_S`, when set, replaces it. `/proc/version` starts with the same
  name.
- `-o` is `Msys`, `-m` is `x86_64` or `aarch64`, `-r` and `-v` are the Windows
  version and build, and `-p` / `-i` are `unknown`, left out of `-a`.
- wbsh exports `MSYSTEM=MINGW64` when the environment has no `MSYSTEM`, and
  sets `OSTYPE` to `msys`.

### Priorities

`nice`, `renice`, `ps` and `top` share one mapping between niceness and Windows
priority classes. REALTIME is never set.

| Niceness | Priority class | Reads back as |
| --- | --- | --- |
| -20..-13 | HIGH | -16 |
| -12..-5 | ABOVE_NORMAL | -8 |
| -4..3 | NORMAL | 0 |
| 4..11 | BELOW_NORMAL | 8 |
| 12..19 | IDLE | 16 |

### top

`top` is procps-style (`-b -n -d -p -u -o -c -i`; keys
`P M N T R c i k r d h q`).

### nohup and setsid

Both start the command on a hidden console of its own, so Ctrl+C, Ctrl+Break
and closing the window leave it running.

- `setsid` keeps a command whose standard streams are still the console on it.
- Ctrl+C while `nohup` or `setsid -w` waits only gives the prompt back.

### sudo and su

wbsh cannot switch users. `sudo` hands the command to the first `sudo` on PATH
(Windows 11 24H2's own, or gsudo) and otherwise explains how to get one. `su`
points to `runas /user:NAME wbsh`.

### script and unbuffer

`script` is util-linux's (`-c`, `-a`, `-e`, `-f`, `-q`, `-I` / `-O` / `-B` log
files) and `unbuffer` is expect's, with `-p`. Both run a command on a
pseudoconsole (ConPTY) of its own, so it sees a console on stdin, stdout and
stderr however wbsh's own streams are redirected, and acts as it would
interactively. `unbuffer cmd | less` and `script -qc cmd /dev/null > out` keep
its colours and line-by-line output.

- The output comes back as the console draws it, with CR LF line ends as from a
  Unix pty.
- The session follows the console's size, and `stty` inside it changes the
  pseudoconsole that every program in it shares.
- Keys typed at a console go through untranslated, Ctrl+C included. Piped input
  is sent as typed lines, ending with Ctrl+Z, Enter (a console's end of file)
  and Ctrl+D.
- `script` runs wbsh, not `$SHELL`, which on Windows usually names another
  environment's shell.
- Both use the `conpty.dll` beside `wbsh.exe` when there is one (Microsoft's
  current ConPTY, as wbshterm does) and Windows' own otherwise.
- Programs built on the Cygwin / MSYS runtime still see a Windows console, not
  their emulated pty.

### crontab and crond

`crontab` is cronie's (`FILE` / `-`, `-l`, `-e` with `$VISUAL` / `$EDITOR` /
`vi`, `-r [-i]`, `-T FILE`) and `crond` is busybox's (`-f`, `-b`, `-l N`,
`-d N`, `-L FILE`, `-c DIR`). Together they are cron for one Windows account.

Crontabs live in `%APPDATA%\wbsh\crontabs\<user>`; `-c DIR` or
`$WBSH_CRONTAB_DIR` moves them. They take lists, ranges, steps, names,
`@reboot` … `@yearly`, `NAME=value` lines and `%`. Daylight-saving changes are
handled as cronie does.

`crond` starts a hidden copy of itself and returns. Jobs run through wbsh
without a window, in `$HOME`. There is no mail, so their output is appended to
`DIR/.crond.log`; `MAILTO=""` discards it and `MAILTO=/some/file` sends it
there.

One crond runs per directory. Stop it with the command below; `pkill crond`
finds nothing, because the process is `wbsh.exe`.

```sh
kill $(cat "$APPDATA/wbsh/crontabs/.crond.pid")
```

wbsh registers nothing with Task Scheduler. To start crond at logon, put a
shortcut to `wbsh.exe -c crond` in `shell:startup`, or run this from cmd:

```bat
schtasks /Create /SC ONLOGON /TN wbsh-crond /TR "\"C:\path\to\wbsh.exe\" -c crond"
```

---

## Network

Listeners bind every interface unless given an address, so the first one may
raise the Windows Firewall prompt. `-s 127.0.0.1` / `-p 127.0.0.1:8080` avoids
it.

**`nc` / `netcat`** connects or listens over TCP and UDP (`-l`, `-k`, `-u`,
`-w`, `-q`, `-N`, `-z` port scans with ranges, `-e` / `-c` to attach a
program).

**`whois`** speaks RFC 3912, following IANA's referral and the thin-registry
"Registrar WHOIS Server" for .com / .net.

**`ftp`** is the classic interactive client: `~/.netrc`, passive EPSV / PASV by
default, active PORT / EPRT via `passive`, and
`ftp ftp://user:pass@host/path` autofetch. busybox's `ftpget` / `ftpput` sit
beside it.

### httpd

`httpd` is a busybox-style static server (`-p [IP:]PORT`, `-h DIR`, `-f`, `-v`).

- **Serves:** GET / HEAD, ranges, If-Modified-Since, directory indexes and
  keep-alive, with path-traversal protection.
- **CGI:** scripts under `/cgi-bin/` run with a 30 s limit per request. `.exe` /
  `.com` / `.bat` / `.cmd` run directly, `.sh` / `.cgi` or `#!` scripts run
  through wbsh, and anything else is refused.
- **Config:** it reads a busybox `httpd.conf`: `A:` / `D:` allow and deny,
  `/path:user:pass` basic auth with plain-text passwords, `.ext:type`,
  `Ennn:/page`, `I:name`.
- **Paths:** request paths are decoded and normalised before the auth and CGI
  checks, and auth rules match case-insensitively, as NTFS names do.

---

## Build

`make` / `gmake` is GNU make 4.4 compatible.

- **Language:** every assignment flavour, `define`, conditionals, `include`,
  remaking of makefiles (main and included) with a restart, and the GNU
  functions (`eval`, `call`, `shell`, `file`, `let`, `intcmp`, ...).
- **Rules:** pattern, static-pattern, double-colon, grouped and suffix rules,
  and GNU's built-in C / C++ rules.
- **Variables and targets:** VPATH / vpath, target- and pattern-specific
  variables, `.SECONDEXPANSION`, `.ONESHELL`, `.DELETE_ON_ERROR` and the other
  special targets.
- **Flags:** `-n -s -k -i -q -t -B -e -r -R -p -C -f -I -o -W -w`, with GNU's
  messages.

Each recipe line runs in a fresh wbsh. `SHELL = /bin/sh` or `bash` means wbsh,
and `.SHELLFLAGS` such as `-eu -o pipefail` reach it as `set` options.
`SHELL = cmd`, or another Windows shell, runs recipes there.

`$(MAKE) -C sub` runs the same make again with MAKEFLAGS and MAKELEVEL passed
down.

`-j` is accepted, but jobs run one at a time, and archive members
(`lib(member)`) are not supported.

---

## Other

**`printf`** has `%b`, `%q` and `-v`.

**`tmux`** is not the real tmux. Under wbshterm it opens pane mode (splits,
status bar, `Ctrl-B` prefix; see
[wbshterm/README.md](../wbshterm/README.md#panes)). Anywhere else it says so
and exits non-zero. There is no server and nothing outlives the window.

**`fzf`** is a fuzzy picker in the spirit of
[junegunn/fzf](https://github.com/junegunn/fzf): pipe lines in, type to narrow,
and Enter prints the selection. With no input it lists the directory tree. It
works mid-pipeline because it talks to the console device directly. Typed bare
at the prompt it acts on the pick: a directory is entered, and a file opens
through its file association. Captured or piped uses keep printing the
selection.

**`man`, `apropos` and `whatis`** show a page for every bundled command and
builtin, page through `$MANPAGER` / `$PAGER` / less, and hand other names to an
external `man` if there is one.

**`locale`** reports LANG / LC_* like glibc, falling back to the Windows user
locale (e.g. `cs_CZ.UTF-8`), and answers `-a`, `-m`, `-k` and keyword queries
from Windows' locale data.
