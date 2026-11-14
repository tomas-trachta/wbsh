# Compatibility ledger: GNU bash 5.3's own test suites

GNU bash ships its regression tests in `tests/`: 86 `run-*` suites, each
running a script and diffing its output against a `.right` file. This page
records how wbsh does on them. It is the strictest measure we have: a suite
counts only if its whole output is **byte-identical**, error messages, line
numbers and the shell's name included.

**2026-11-13, local: 11/86 byte-identical, 0 env-only, 75 real failures
(4 of them timeouts).**

- Shell: wbsh 1.1.1, `x64/Release/wbsh.exe` built 2026-11-13 23:07 from
  0d0f1b6 plus the uncommitted `feat/drop-in-roadmap` work.
- Reference: bash-5.3.tar.gz from ftp.gnu.org, SHA-256
  `0d5cd86965f869a26cf64f4b71be7b96f90a3ba8b3d74e27e8e9d9d5550f31ba`.
- Machine: Windows 10 22H2 (19045), Ryzen 7 5800H; 2 suites in parallel,
  below-normal priority, 120 s per suite.
- Harness: [`tools/bashtests/run.py`](../tools/bashtests/README.md), which
  also explains how the suites are run and how env-only is decided
  ([`env_rules.json`](../tools/bashtests/env_rules.json)).
- CI: the `bashtests` job in
  [`.github/workflows/ci.yml`](../.github/workflows/ci.yml) runs the same
  thing on every push and uploads `bashtests-results.json` and the diffs.

```sh
python -I tools/bashtests/run.py --shell x64/Release/wbsh.exe --fetch C:/cache/bash-src \
    --jobs 2 --timeout 120 --low-priority --json bashtests.json --diff-dir diffs
```

This is the first measurement, so the number is low and honest. None of the
75 failures here was put down to the environment: the env rules only cover
umask, missing POSIX locales, Windows file-name characters and Linux signal
numbers, and every failing suite also has differences outside those.

## Identical (11)

`extglob2`, `extglob3`, `herestr`, `ifs-posix`, `invert`, `iquote`,
`lastpipe`, `nquote1`, `nquote5`, `precedence`, `strip`.

## Real failures (75)

"Changed" is the number of added plus removed lines in the diff. The reason
is the first or the dominant difference; most suites have several. The ids
in brackets refer to the reproducers in [Shell bugs found](#shell-bugs-found).

| suite | changed | reason |
| --- | ---: | --- |
| `alias` | 113 | an alias whose value contains an array assignment (`m=("x")`) runs `m=` as a command [B6]; `alias '\$'` accepted; later output lost |
| `appendop` | 28 | `+=` on integer variables and on array elements inside `(( ))` (`a[2]+=7`) evaluates wrongly |
| `arith` | 174 | several arithmetic error messages differ or are missing (`2#`, `jv += $iv`, invalid number vs base); takes 100 s |
| `arith-for` | 48 | `for (( ; "i < 3"; ))` with a quoted expression rejected [B9]; `for ((` printed back without bash's spacing |
| `array` | 852 | a syntax error in an array literal aborts the script; bash reports it and goes on with the next command [B11] |
| `array2` | 6 | unquoted `${A[*]}` with `IFS=` (empty) is joined into one word instead of one word per element [B20] |
| `assoc` | 417 | associative-array element order differs from bash's hash order; `declare -A x` prints `x=()` [B16] |
| `attr` | 20 | readonly errors lack the function / builtin name prefix (`f2: a: readonly variable`); `export -a` unknown |
| `braces` | 40 | `{3..3}` not expanded [B13]; `{a..f}`-style ranges over punctuation; `$(\` newline in a brace word |
| `builtins` | 425 | **timeout**: `echo 'echo x' \| . /dev/stdin` hangs [B21]; umask, `enable` (no loadables) differ before that |
| `case` | 2 | `shopt -s -o posix` (`-o` with `shopt`) rejected |
| `casemod` | 22 | case-modification of associative arrays prints elements in a different order (hash order) |
| `complete` | 751 | `complete -p` lists compspecs in a different order and some option spellings differ |
| `comsub` | 29 | `$( )` text with escaped newlines / quotes kept literally in some forms; several later checks fail |
| `comsub-eof` | 21 | here-document inside `$( )` terminated by EOF: bash warns and continues, wbsh errors |
| `comsub-posix` | 31 | syntax errors inside `$( )` worded differently (`expected 'fi'`) and do not abort the command |
| `comsub2` | 82 | `${ cmd; }` (bash 5.3 funsub): `return` inside it is rejected [B17]; `local` inside it rejected |
| `coproc` | 10 | **timeout**: coproc file descriptors are 10/11 instead of 63/60, later coprocs hang |
| `cprint` | 2 | `declare -f` prints `$( echo 1 2 3 )` with extra spaces |
| `dbg-support` | 86 | `caller` option handling, `BASH_LINENO` / `LINENO` inside DEBUG traps in functions |
| `dbg-support2` | 2 | DEBUG trap with `extdebug` return value not honoured (`x is 2` vs `1`) |
| `dirstack` | 33 | `pushd /usr` fails (no `/usr`; env), and `dirs`/`~N` output differs after that (real) |
| `dollars` | 141 | unquoted `$*` / `${A[*]}` with an empty IFS joined into one word [B20], and further `$@`/`$*` corners |
| `dynvar` | 7 | assigning `BASH_ARGV0` does not change `$0` [B22]; `GROUPS` assignment error |
| `errors` | 379 | builtin usage errors (`unalias -x` [B18], invalid identifiers, `exit abcde` [B3]) not reported or worded differently |
| `execscript` | 125 | `/bin/sh` and `/tmp/bash` do not exist (env), and exec-failure messages and statuses differ (real) |
| `exp-tests` | 811 | `"`echo \"Hello world!\"`"` keeps the backslash-quotes [B7]; unquoted `**` matched files |
| `exportfunc` | 18 | exported functions (`BASH_FUNC_foo%%`) are not imported by child shells |
| `extglob` | 71 | some extglob patterns with brackets match differently; `touch a:b` fails (env, file name) |
| `func` | 230 | `compgen -A function` / `unset -f $funcs` leave functions defined, so `declare -f` lists them |
| `getopts` | 5150629 | **timeout**: `getopts` with a `local OPTIND` in a recursive function loops forever [B4] |
| `glob-bracket` | 103 | bracket expressions containing `/` in patterns, and `$GLOBIGNORE` vs fnmatch, differ |
| `glob-test` | 117 | non-UTF-8 bytes (`\243`) in patterns and file names are converted instead of kept as bytes |
| `globstar` | 717 | `**` returns the same paths in a different order |
| `heredoc` | 179 | backslash-newline is not joined before the here-document delimiter check [B10] |
| `histexpand` | 262 | history expansion is not available in non-interactive shells with `set -H` |
| `history` | 455 | `history -r/-w`, `fc -l` on a history file in scripts unsupported |
| `ifs` | 12 | `IFS=:` while assigning `x=$(echo a:b:c)` stores `a b c` [B1] |
| `input-test` | 2 | a script read from stdin is buffered, so `read` in it gets nothing [B23] |
| `intl` | 66 | multibyte handling under `LANG=en_US.UTF-8` (`${#x}`, `printf %d "'é"`, byte escapes) differs, output stops early |
| `invocation` | 110 | `--badopt` / `-c` without argument errors and the `--help` text differ; shell names itself `wbsh` when run as `bash.exe` [B2] |
| `jobs` | 131 | **timeout**: job-control output format (`jobs` columns, `( sleep 2; exit 4 )` printing), `fg` without job control |
| `mapfile` | 174 | `mapfile -C callback -c N` unsupported [B8] |
| `more-exp` | 91 | `eval $1=\"\${$1:-$2}\"`-style word splitting / quoting in parameter defaults |
| `nameref` | 387 | nameref corner cases: `unset -n`, readonly through a reference, arrays as references |
| `new-exp` | 806 | a quoting parse error at line 666 aborts the rest; `${HOME:}` error text differs |
| `nquote` | 31 | `$'...'` inside `"${...}"` and `'` handling inside double-quoted expansions |
| `nquote2` | 64 | `\001` (CTLESC) bytes in parameter expansions and pattern replacement |
| `nquote3` | 76 | `\001` bytes in `${x#...}` / `${x%...}` patterns |
| `nquote4` | 24 | `$'\x..'` / `\u` escapes producing non-UTF-8 bytes are truncated (`<ab>`) |
| `parser` | 19 | syntax error messages worded differently (`expected ')'`); `select` prompt on stdout |
| `posix2` | 6 | 4 of 27 POSIX.2 checks fail: `OPTIND` initial value, three variable-quoting cases |
| `posixexp` | 297 | `${x-'bar'}` and brace/quote nesting inside `${...}` in POSIX mode |
| `posixexp2` | 41 | `${x:-}z}`-style unbalanced braces/quotes inside expansions |
| `posixpat` | 7 | several POSIX bracket expressions (`[[:alpha:][:digit:]]`, equivalence classes) do not match |
| `posixpipe` | 46 | a bare `!` (empty negated pipeline) is a syntax error [B19]; `time` without a pipeline |
| `printf` | 131 | `printf -v invalid-var`, `%Q` (bash 5.3), `%n`, option parsing errors differ |
| `procsub` | 24 | `eval cat <(...)` fails: the pipe path `\\.\pipe\...` loses its backslashes under `eval` [B24] |
| `quote` | 78 | backslash-newline inside double quotes and in `$( )`; an unterminated quote in a sourced file |
| `quotearray` | 170 | `assoc_expand_once` / quoted subscripts with `$( )` in `(( ))` evaluated differently |
| `read` | 30 | `read` with fewer fields than names keeps a trailing separator; non-UTF-8 bytes shown as `\xcd` |
| `redir` | 180 | `exec -1`-style bad fds, `$z` expanding to two words is not an ambiguous redirect [B25] |
| `rhs-exp` | 20 | `\'` and `$var` inside `"${x:-...}"` right-hand sides |
| `rsh` | 33 | restricted shell (`set -r`, `bash -r`) not implemented [B15]; `/bin/sh` missing (env) |
| `set-e` | 1 | one `set -e` case in a compound command exits where bash continues |
| `set-x` | 52 | `set -x` does not trace `(( ))` inside `for ((...))` and some compound commands |
| `shopt` | 182 | `shopt` option list and `-o` handling; `globstar` defaults on in wbsh |
| `test` | 75 | file tests on setgid/sticky bits and owner/group (`-G`, `-O`, `chgrp`) (partly env), plus string/arith test corners |
| `tilde` | 6 | `/usr` and `/tmp` do not exist (env); `foo=bar:~` should not expand after `:` here (real) |
| `tilde2` | 8 | tilde expansion in assignment words and `HOME=~` corner cases |
| `trap` | 62 | DEBUG trap: `${BASH_SOURCE}` reads `main`, `declare -f` printed by `trap -p` |
| `type` | 50 | `type -t` of a hashed external, `/bin/sh` resolves to Git's `sh.exe` (env) |
| `varenv` | 236 | `set -k` (keyword arguments) unsupported [B5]; function-local environment scoping |
| `vredir` | 26 | `{v}` redirections: closing `{v}>&-` printed as `<&-`, error wording |

## Shell bugs found

Reproducers checked on 2026-11-13 against the build above (B12, B14, B28 and B29 came from the [real-world corpus](corpus/2026-11-13-local.md)); "bash" is GNU
bash 5.3's `.right` file where the behaviour is 5.x-only, otherwise Git
Bash 4.4 agreed. Each is a one-liner for `wbsh -c '...'` or a script file.

| id | reproducer | bash | wbsh |
| --- | --- | --- | --- |
| B1 | `IFS=:; x=$(echo a:b); IFS=' '; echo "$x"` | `a:b` | `a b` (the value was split and re-joined) |
| B2 | copy `wbsh.exe` to `bash.exe`; `bash.exe -c 'echo $0; nosuch'` | `bash`, `bash: line 1: nosuch: command not found` | `wbsh`, `wbsh: line 1: ...` |
| B3 | `exit abcde` | `exit: abcde: numeric argument required`, status 2 | silent, status 0 (`shift abcde` also silent) |
| B4 | `f(){ local OPTIND=1 opt; while getopts ":abcxyz" opt; do echo $opt; [[ $opt = y ]] && f -abc; done; }; f -xyz` | `x y a b c z` | loops forever |
| B5 | `set -k; f(){ echo "a=$a"; }; f a=5` | `a=5` | `a=` |
| B6 | script: `shopt -s expand_aliases` / `alias L='m=("x")'` / `L` / `declare -p m` | `declare -a m=([0]="x")` | `m=: command not found` |
| B7 | ``echo "`echo \"Hello world!\"`"`` | `Hello world!` | `"Hello world!"` |
| B8 | `printf 'a\nb\n' \| { mapfile -C 'echo cb' -c 1 arr; }` | `cb 0 a`, `cb 1 b` | `mapfile: unknown option: -C` |
| B9 | `for (( i=0; "i < 3"; i++ )); do echo $i; done` | `0 1 2` | `((: "i < 3": syntax error` |
| B10 | script: `cat <<EOF` / `next\` / `EOF` / `EOF` | `nextEOF` | `next`, then `EOF: command not found` |
| B11 | script: `echo before` / `test=(first & second)` / `echo after` | error message, then `after`, status 0 | aborts with status 2 |
| B12 | script: `[[ a == a &&` / `   -z "" ]] && echo ok` | `ok` | `expected operand in [[ ... ]]` (breaks bash-completion and git-completion.bash) |
| B13 | `echo {3..3} x{3..3}y` | `3 x3y` | `{3..3} x{3..3}y` |
| B14 | `mkdir d; echo 'echo hi' > t.sh; ln -s ../t.sh d/l.sh; cat d/l.sh` | `echo hi` | `cat: d/l.sh: Invalid argument` (the link target keeps `/`; breaks bash-it) |
| B15 | `set -r; cd /; echo $?` | `cd: restricted`, `1` | `set: -r: invalid option`, `0` |
| B16 | `declare -A x; declare -p x` | `declare -A x` | `declare -A x=()` |
| B17 | `echo ${ printf '%s\n' aa bb; return; echo cc; }` (5.3) | `aa bb` | `return: can only 'return' from a function` |
| B18 | `unalias -x foo` | `unalias: -x: invalid option` + usage, status 2 | `unalias: foo: not found`, status 1 |
| B19 | `!; echo $?; ! !; echo $?` | `1`, `0` | `expected command after '!'` |
| B20 | `IFS=; A=(bob 'tom dick' joe); set -- ${A[*]}; echo $#; set -- a b; set -- $*; echo $#` | `3`, `2` (with empty IFS each element stays a word) | `1`, `1` (joined into one word) |
| B21 | `echo 'echo three' \| . /dev/stdin` | `three` | hangs |
| B22 | `BASH_ARGV0=hello; echo "$0"` (5.x) | `hello` | unchanged |
| B23 | `printf 'read x\nhello\necho "[$x]"\n' \| wbsh` | `[hello]` | `[]`, then `hello: command not found` |
| B24 | `eval cat <(echo hi)` | `hi` | `cat: \.pipewbsh-...: No such file or directory` |
| B25 | `z='a b'; echo hi > $z; echo $?` | `$z: ambiguous redirect`, `1` | writes a file named `a b`, `0` |
| B26 | `hash -p /bin/true mytrue; hash -t mytrue` | `/bin/true` | prints nothing |
| B27 | `wbsh -i script.sh` (or `-i -c`) with `echo $-` in it | contains `i` | no `i`: `-i` only takes effect for the REPL |
| B28 | `eval -- "a=(x y)"; echo ${#a[@]}` | `2` | `syntax error near unexpected token '('` (`--` is taken as a word; breaks every oh-my-bash load) |
| B29 | `shopt -qo errexit; echo $?` | `1` | `shopt: unknown option: -qo` (clustered options) |

## Refreshing this page

Run the command above against a fresh build, replace the date, the totals,
the identical list and the table, and re-check the reproducers. When a
suite becomes identical, move it up; when its remaining differences are all
environmental, add an entry with a reason to `suite_rules` in
`tools/bashtests/env_rules.json` instead of editing this page by hand.
