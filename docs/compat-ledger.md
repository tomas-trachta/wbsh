# Compatibility ledger: GNU bash 5.3's own test suites

GNU bash ships its regression tests in `tests/`: 86 `run-*` suites, each
running a script and diffing its output against a `.right` file. This page
records how wbsh does on them. It is the strictest measure we have: a suite
counts only if its whole output is **byte-identical**, error messages, line
numbers and the shell's name included.

**2026-11-23, local: 68/86 byte-identical, 18 env-only, no real failures,
no timeouts.** The first measurement, on 2026-11-13, was 11/86.

- Shell: wbsh 1.1.3, `x64/Release/wbsh.exe` built 2026-11-23 from c9532f2
  plus the rest of the `fix/bash-compat` work.
- Reference: bash-5.3.tar.gz from ftp.gnu.org, SHA-256
  `0d5cd86965f869a26cf64f4b71be7b96f90a3ba8b3d74e27e8e9d9d5550f31ba`.
- Machine: Windows 10 22H2 (19045), Ryzen 7 5800H; 3 suites in parallel,
  below-normal priority, 150 s per suite.
- Harness: [`tools/bashtests/run.py`](../tools/bashtests/README.md), which
  also explains how the suites are run and how env-only is decided
  ([`env_rules.json`](../tools/bashtests/env_rules.json)).
- CI: the `bashtests` job in
  [`.github/workflows/ci.yml`](../.github/workflows/ci.yml) runs the same
  thing on every push and uploads `bashtests-results.json` and the diffs.

```sh
python -I tools/bashtests/run.py --shell x64/Release/wbsh.exe --fetch C:/cache/bash-src     --jobs 3 --timeout 150 --low-priority --json bashtests.json --diff-dir diffs
```

## Identical (68)

`alias`, `appendop`, `arith`, `arith-for`, `array`, `array2`, `assoc`, `attr`, `braces`, `case`, `casemod`, `complete`, `comsub`, `comsub-eof`, `comsub-posix`, `comsub2`, `cond`, `cprint`, `dbg-support`, `dbg-support2`, `dollars`, `dynvar`, `errors`, `exp-tests`, `exportfunc`, `extglob2`, `extglob3`, `func`, `getopts`, `globstar`, `herestr`, `ifs`, `ifs-posix`, `input-test`, `intl`, `invert`, `iquote`, `jobs`, `lastpipe`, `mapfile`, `more-exp`, `nameref`, `nquote`, `nquote1`, `nquote2`, `nquote3`, `nquote4`, `nquote5`, `parser`, `posix2`, `posixexp`, `posixexp2`, `posixpat`, `posixpipe`, `precedence`, `printf`, `procsub`, `quote`, `quotearray`, `rhs-exp`, `set-e`, `set-x`, `shopt`, `strip`, `tilde2`, `trap`, `varenv`, `vredir`.

## Env-only (18)

Every changed line of these suites is either the machine or one of
wbsh's stated design choices, never a shell bug. Each rule in
[`env_rules.json`](../tools/bashtests/env_rules.json) names what was read
and what matches.

From the machine: a Linux path that does not exist on Windows, a file name
Windows rejects, or the test harness's closed stdin.

| suite | rule | what differs |
| --- | --- | --- |
| `coproc` | `coproc-no-etc-passwd` | `cat /etc/passwd` finds no file |
| `dirstack` | `dirstack-unix-dirs` | `/usr`, `/bin` and friends do not exist |
| `extglob` | `extglob-colon-filename` | `touch a:b` fails on Windows |
| `glob-bracket` | `glob-bracket-needs-build-tree` | needs bash's build tree and a C compiler |
| `glob-test` | `glob-windows-filesystem` | file names Windows rejects or folds |
| `heredoc` | `bash-build-tree-files`, `host-grep-message`, `windows-reserved-filename` | bash's build tree, the host grep's wording, a file name Windows reserves |
| `histexpand` | `histexpand-git-sh` | `/bin/sh` is Git's sh, which calls itself `/usr/bin/sh` |
| `read` | `read-stdin-closed` | the timed reads see the harness's closed stdin |
| `redir` | `redir-unix-files` | `/etc`, `/dev/tty` and Unix device files |
| `rsh` | `rsh-no-bin-sh` | `cp /bin/sh .` has no `/bin/sh` to copy |
| `test` | `test-unix-file-modes` | setgid, sticky and owner checks on Unix files |
| `tilde` | `tilde-no-usr` | `cd /usr` fails; `/tmp` maps to the temp directory |
| `type` | `type-windows-commands` | `/bin/sh` and the built-in `cat` |

From a design choice, each next to machine differences like the above:

| suite | rule | what differs |
| --- | --- | --- |
| `builtins` | `builtins-gpl-help-and-unix-dirs` | bash's `help` text is GPL and wbsh is MIT, so wbsh keeps its own; no `/bin`, the `ulimit -u` hard limit |
| `execscript` | `execscript-no-exec-bit` | Windows files have no execute bit, so a script without one still runs; macOS signal order in `exec.right`, no `sh` on the PATH, the built-in `cat` |
| `history` | `history-version-and-file-format` | `BASH_VERSION` is 5.2; `bash -i` writes wbsh's own history file, which keeps each entry's exit status |
| `invocation` | `invocation-wbsh-startup-files` | a login shell reads only `~/.wbsh_*`, not `~/.bash_logout`; no `/usr/bin` |
| `new-exp` | `new-exp-version-and-root` | `BASH_VERSION` is 5.2; `cd /` reaches `/c/` |

## Real failures (0)

None. The last ones fixed, from 2026-11-22: `declare -n` with other
attributes and circular references (`nameref`); a here-document opened
inside `$( )` (`heredoc`); history recorded and expanded line by line in
scripts, `fc` ranges, history files with timestamps and `bash -i` reading
a pipe (`history`, `histexpand`); `set -e` ignored by pipeline elements
and coprocs under `!`, and the EXIT trap after `set -e` running with the
function's redirections undone (`execscript`).

## Shell bugs found

The 29 reproducers recorded on 2026-11-13 (B1 to B29; B12, B14, B28 and
B29 came from the [real-world corpus](corpus/2026-11-13-local.md)) were
re-checked on 2026-11-22 against the build above: wbsh now gives bash's
output for all of them. Each is a one-liner for `wbsh -c '...'` or a
script file.

| id | reproducer | bash and wbsh now |
| --- | --- | --- |
| B1 | `IFS=:; x=$(echo a:b); IFS=' '; echo "$x"` | `a:b` |
| B2 | copy `wbsh.exe` to `bash.exe`; `bash.exe -c 'echo $0; nosuch'` | `bash`, `bash: line 1: nosuch: command not found` |
| B3 | `exit abcde` | `exit: abcde: numeric argument required`, status 2 |
| B4 | `f(){ local OPTIND=1 opt; while getopts ":abcxyz" opt; do echo $opt; [[ $opt = y ]] && f -abc; done; }; f -xyz` | `x y a b c z` |
| B5 | `set -k; f(){ echo "a=$a"; }; f a=5` | `a=5` |
| B6 | script: `shopt -s expand_aliases` / `alias L='m=("x")'` / `L` / `declare -p m` | `declare -a m=([0]="x")` |
| B7 | ``echo "`echo \"Hello world!\"`"`` | `Hello world!` |
| B8 | `printf 'a\nb\n' \| { mapfile -C 'echo cb' -c 1 arr; }` | `cb 0 a`, `cb 1 b` |
| B9 | `for (( i=0; "i < 3"; i++ )); do echo $i; done` | `0 1 2` |
| B10 | script: `cat <<EOF` / `next\` / `EOF` / `EOF` | `nextEOF` |
| B11 | script: `echo before` / `test=(first & second)` / `echo after` | error message, then `after`, status 0 |
| B12 | script: `[[ a == a &&` / `   -z "" ]] && echo ok` | `ok` |
| B13 | `echo {3..3} x{3..3}y` | `3 x3y` |
| B14 | `mkdir d; echo 'echo hi' > t.sh; ln -s ../t.sh d/l.sh; cat d/l.sh` | `echo hi` |
| B15 | `set -r; cd /; echo $?` | `cd: restricted`, `1` |
| B16 | `declare -A x; declare -p x` | `declare -A x` |
| B17 | `echo ${ printf '%s\n' aa bb; return; echo cc; }` (5.3) | `aa bb` |
| B18 | `unalias -x foo` | `unalias: -x: invalid option` + usage, status 2 |
| B19 | `!; echo $?; ! !; echo $?` | `1`, `0` |
| B20 | `IFS=; A=(bob 'tom dick' joe); set -- ${A[*]}; echo $#; set -- a b; set -- $*; echo $#` | `3`, `2` |
| B21 | `echo 'echo three' \| . /dev/stdin` | `three` |
| B22 | `BASH_ARGV0=hello; echo "$0"` (5.x) | `hello` |
| B23 | `printf 'read x\nhello\necho "[$x]"\n' \| wbsh` | `[hello]` |
| B24 | `eval cat <(echo hi)` | `hi` |
| B25 | `z='a b'; echo hi > $z; echo $?` | `$z: ambiguous redirect`, `1` |
| B26 | `hash -p /bin/true mytrue; hash -t mytrue` | `/bin/true` |
| B27 | `wbsh -i script.sh` (or `-i -c`) with `echo $-` in it | contains `i` |
| B28 | `eval -- "a=(x y)"; echo ${#a[@]}` | `2` |
| B29 | `shopt -qo errexit; echo $?` | `1` |

## Refreshing this page

Run the command above against a fresh build, replace the date, the totals,
the identical list and the table, and re-check the reproducers. When a
suite becomes identical, move it up; when its remaining differences are all
environmental, add an entry with a reason to `suite_rules` in
`tools/bashtests/env_rules.json` instead of editing this page by hand.
