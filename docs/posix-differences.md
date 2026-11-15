# Differences from POSIX environments

wbsh is a native Windows program. It runs bash scripts, but it does not
bring a POSIX runtime the way Git Bash, MSYS2 and Cygwin do, and it is not
Linux the way WSL is. This page lists what that means in practice: which
POSIX facilities wbsh emulates, how, and where the emulation stops.
Differences in the bash language itself (error wording, parser corners) are
in the [compatibility ledger](compat-ledger.md), not here.

Checked against wbsh on Windows 10 22H2 on 2026-11-15.

## Processes: there is no fork

- Subshells, `( ... )` and command substitutions run **inside the shell
  process**, on a copy of its state. They are cheap, but `$BASHPID` in one
  is a synthetic number that no real process has.
- Pipeline stages that are shell code (functions, compound commands,
  builtins) run in a **child wbsh** that receives the shell's functions,
  aliases, arrays and options serialized in its environment. Its `$BASHPID`
  and `/proc/self` are its own, as in bash.
- External programs start with `CreateProcess`. `$$`, `$!` and `ps` show
  real Windows process IDs.
- `exec COMMAND` cannot replace the process. It starts the command, waits
  for it, and the shell then exits with the command's exit status; the PID
  the parent knew is the shell's, not the command's. Processes the command
  left behind keep running.

## Signals

Windows has no signals. wbsh emulates them where it can:

- **Between wbsh processes.** Every wbsh has a signal mailbox. `kill`,
  `pkill`, `killall` and `timeout` post to it, and the receiving shell acts
  on the signal at its next safe point (between commands, inside `wait` or
  `sleep`): it runs the trap, ignores the signal, or ends with status
  128 + n as an uncaught signal would. A shell that is busy inside a long
  builtin is ended after a short grace if it does not trap the signal.
- **Any other process** cannot catch anything. `STOP`/`TSTP` suspend it
  (`NtSuspendProcess`), `CONT` resumes it and signal 0 only checks that it
  exists. Every other signal, `TERM` and `INT` included, terminates it
  immediately with exit code 128 + n, without a chance to clean up.
  `timeout -k` first sends a Ctrl+Break, the one thing such a program can
  survive.
- **Ctrl-C** is the console's Ctrl+C event. It reaches every process on the
  console at once, and wbsh runs its `INT` trap. **Ctrl-Z** is handled by
  wbsh, not by a terminal driver: it suspends the foreground job, and
  `jobs`, `fg`, `bg` and `kill -CONT` work as in bash.
- Signals the shell raises for itself: `EXIT`, `ERR`, `DEBUG`, `RETURN`,
  `INT` on Ctrl-C, and `WINCH` when the console is resized at an
  interactive prompt. `CHLD`, `HUP`, `PIPE`, `ALRM` and the rest only
  arrive when another wbsh sends them with `kill`.

## Users, groups and permissions

- There is no uid/gid model. `id` reports the current Windows account as
  uid and gid 1000; there is no `/etc/passwd` or `/etc/group`. `~name`
  expands to that Windows user's profile directory.
- `chown` and `chgrp` set the file's real NTFS owner and group (an account
  name, a SID or a number), which usually needs administrator rights.
  `ls -l` and `stat` still show the current user as owner of every file.
- `chmod`, `test -x`, `[[ -x ]]`, `ls -l`, `stat`, `find -perm` and `tar`
  share one permission model:
  - the owner write bit is the read-only attribute;
  - the other bits are kept in an NTFS alternate data stream, `wbsh.mode`,
    when they differ from the default;
  - by default a directory is 0755, a file is 0755 when its name ends in
    `.exe`, `.com`, `.bat`, `.cmd`, `.ps1` or `.sh` or it starts with `#!`
    or `MZ`, and 0644 otherwise.

  Only wbsh and its bundled commands read the stream. Windows, Git and other
  programs do not, the group and other bits restrict nobody (NTFS ACLs
  decide who can open a file), and copying a file to FAT, exFAT or some
  network shares loses the stream.
- setuid, setgid and the sticky bit do not exist: `-u`, `-g` and `-k` are
  always false. `su` and `sudo` point to Windows' own elevation tools
  instead of switching users.

## File system

- **Case-insensitive names.** `Makefile` and `makefile` are the same file.
- **Reserved names and characters.** File names cannot contain
  `: * ? " < > |`, and `CON`, `PRN`, `AUX`, `NUL`, `COM1`–`COM9` and
  `LPT1`–`LPT9` name devices, on Windows 10 even with an extension
  (`nul.txt`). Trailing dots and spaces are dropped.
- **Open files are locked.** A file another process has open, such as a
  running `.exe`, usually cannot be removed or renamed.
- **Symlinks.** `ln -s` creates a real Windows symlink, which needs
  Developer Mode or the symlink privilege; otherwise it fails rather than
  copying. Whether the link is a file or directory link is decided when it
  is made, so make it after its target exists.
- **Paths.** Drives appear as `/c/...`. There is no `/usr`, `/bin` or
  `/etc` tree. Arguments that look like POSIX paths are converted to Windows
  paths for external programs, as MSYS2 does (`WBSH_NO_PATHCONV` or
  `MSYS_NO_PATHCONV` turns that off); a path inside a longer string, such
  as `cmd //c "type /dev/null"`, is not converted.
- **Emulated special files are wbsh's own.** `/dev/null`, `/dev/zero`,
  `/dev/stdin` and the other `/dev` paths, `/proc` and FIFOs made by
  `mkfifo` (a marker file backed by a named pipe) work in wbsh and its
  bundled commands. Other programs see none of it: to them a FIFO is a
  plain marker file and `/proc` does not exist. The one exception is
  `/dev/null` given as an argument, which becomes `nul`.

## Terminals

- wbsh runs in a Windows console (conhost, Windows Terminal) or in wbshterm.
  The programs it starts share that console. They do not get a pty or a
  termios device; `stty` is a wbsh builtin that changes the console's
  modes.
- `script` and `unbuffer` run a command on a pseudo console (ConPTY), so it
  sees a terminal while its output goes to a file or a pipe.

## Not a POSIX runtime

wbsh runs Windows programs. It cannot compile or run unmodified Unix
programs, because there is no POSIX C library for them to link against, and
there is no package manager: the commands it bundles are builtins, and
anything else comes from Windows, Git for Windows, MSYS2, Scoop or winget.
MSYS2 and Cygwin programs run fine as children, since they bring their own
runtime. When you need a POSIX runtime, use MSYS2 or Cygwin; when you need
Linux, use WSL.

`uname -s` reports `MINGW64_NT-…`, so scripts take the branch they already
have for Git Bash. `WBSH_UNAME_S` overrides it.
