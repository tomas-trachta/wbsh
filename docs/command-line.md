# Command line

How to run wbsh, the options it takes, and the two modes written for
callers that are programs rather than people.

## Usage

```
wbsh [options]                         interactive shell (TTY auto-detect)
wbsh [options] -c <command> [name [arg ...]]
                                       run the command ($0 = name)
wbsh [options] <file> [arg ...]        run the file ($1.. = args)
wbsh [options] -s [arg ...]            run stdin (also a lone -)
wbsh [options] --ec <base64> [name [arg ...]]
wbsh --json [options] -c <command>     one JSON line: status, stdout, stderr

  -c, -s, -i, -l                       as in bash
  -abefhkmnptuvxBCEHPT                 the `set` letters (+x turns one off)
  -o NAME / +o NAME, -O NAME / +O NAME set / unset by name, shopt names
  --login, --posix, --norc, --noprofile, --rcfile FILE, --noediting
  --tokens, --ast, --expand            debugging dumps instead of running
                                       (add -r / --run to dump and run)
  -h, --help                           show help
  --version                            version and the commit it was built from
  --agent-info                         a longer brief for scripts and tools
```

## Drop-in for bash.exe and sh.exe

The command line is bash's: option letters cluster (`-ec`), `-c` takes the
first operand after the options (`wbsh -c -l 'cmd'`), and `-o NAME`,
`-O NAME`, `-l`, `-i`, `-s`, `--norc`, `--noprofile`, `--rcfile`, `--noediting` and
`--posix` mean what they mean to bash. A copy of `wbsh.exe` named `bash.exe`
behaves the same; one named `sh.exe` starts in POSIX mode. Tools that run
`$SHELL -c`, git hooks and Makefiles therefore work unchanged, and a coding
agent that wants Git Bash can be pointed at it (for Claude Code:
`CLAUDE_CODE_GIT_BASH_PATH=C:\path\to\bash.exe`).

A login shell (`-l`) reads `~/.wbsh_profile`, never the `~/.profile` written
for Git Bash or Linux; an interactive one then reads `~/.wbshrc` too.

## Quote-proof commands

Every program between a caller and the shell can re-quote, expand or eat
part of a command line. `--ec` (or `--encodedcommand`) takes the command as
base64 instead, so there is nothing left to escape:

```sh
$ wbsh --ec ZWNobyAiaGkgJDEiIHwgdHIgYS16IEEtWg== name arg
HI ARG
```

UTF-8 and UTF-16LE (PowerShell's `-EncodedCommand` encoding) are both
accepted; padding is optional, line breaks are ignored and an optional `v1:`
prefix is dropped. Options still go in front: `wbsh -eu --ec <b64>`.

## Structured results

`--json` runs the command the normal way (traps, `exit N`, `set -e`) with
stdout and stderr captured apart, then prints one line of JSON, so a caller
needs no output parsing:

```sh
$ wbsh --json -c 'echo hi; echo oops >&2; exit 3'
{"status":3,"stdout":"hi\n","stderr":"oops\n","duration_ms":4}
```

The line is pure ASCII (non-ASCII characters as `\uXXXX`, so it parses
whatever code page the reader decodes it with); bytes that are not valid
UTF-8 become U+FFFD, one per byte. The exit status is the command's. It
combines with `--ec` and script files: `wbsh --json --ec <b64>`.

Coding agents: [agents.md](agents.md) shows how to point Claude
Code, Codex CLI, OpenCode and others at wbsh, and what
`tools/agents/check_agent_shapes.py` verifies about each of them.
