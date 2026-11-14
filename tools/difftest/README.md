# Differential tests

A corpus of small bash scripts that run under GNU bash and under wbsh, with
the outputs compared after a fixed set of normalizations. GNU bash is the
reference: whatever bash prints is, by definition, right.

```
tools/difftest/
    run.py          the runner (Python 3.8+, stdlib only, safe under python -I)
    cases/<area>/<name>.sh
    baseline.txt    minimum pass count CI accepts
```

## Running it

Record reference outputs with bash, then compare wbsh against them:

```sh
python -I tools/difftest/run.py --shell /usr/local/bin/bash --record ref/
python -I tools/difftest/run.py --shell x64/Release/wbsh.exe --compare ref/
```

Useful options:

| option | effect |
| --- | --- |
| `--filter 'glob/*'` | only cases whose `area/name` matches; repeatable |
| `--diff` | print a unified diff for every mismatch |
| `--diff-dir DIR` | write `DIR/<area>/<name>.diff` per mismatch |
| `--json FILE` | per-case results as JSON |
| `--summary-md FILE` | append a markdown summary (CI passes `$GITHUB_STEP_SUMMARY`) |
| `--baseline FILE` | exit 1 if fewer cases pass than the number in FILE |
| `--quiet` | leave passing cases out of the table |
| `--ignore-line-numbers` | drop `line N: ` from error messages on both sides |
| `--jobs N` | cases run in parallel (default 4) |
| `--timeout S` | seconds per case (default 10) |
| `--low-priority` | run the runner and every shell below normal priority |
| `--keep`, `--work-root DIR` | keep the per-case working directories |
| `--list` | list the cases and their version requirements |

A compare run exits 0 however many cases mismatch; it exits 1 only when the
pass count is below `--baseline`. The point for now is to report, not to
gate.

## How a case runs

Each case is copied to `case.sh` in a fresh, empty directory and run as
`<shell> case.sh` with that directory as the working directory and stdin
closed. stdout, stderr and the exit status are captured separately, so their
interleaving never matters. The environment is the caller's, minus the
variables that change shell behaviour (`BASH_ENV`, `ENV`, `CDPATH`,
`GLOBIGNORE`, `POSIXLY_CORRECT`, `SHELLOPTS`, `BASHOPTS`, `IFS`, `PS4`,
`BASH_COMPAT`, every `LC_*`, exported functions and a few more listed in
`SCRUBBED_VARIABLES`), plus `LC_ALL=C`, `LANG=C`, `TZ=UTC`, `TERM=dumb` and
`HOME` set to the working directory.

Each case is classified as:

| category | meaning |
| --- | --- |
| `pass` | status, stdout and stderr all identical |
| `stderr` | status and stdout identical, stderr differs (usually message wording) |
| `fail` | status or stdout differs |
| `timeout` | the shell under test did not finish in time |
| `no-ref` | no recorded reference for this case |
| `ref-too-old` | the case needs a newer bash than the one that recorded the reference; not counted |

The pass rate is `pass / (pass + stderr + fail + timeout)`.

## Normalizations

These are applied to stdout and stderr of both shells, in this order, and are
the only ones. Anything else that differs is a real difference.

1. **Line endings.** `CRLF` becomes `LF`. wbsh and native tools may write
   CRLF on Windows; bash on Linux never does.
2. **The working directory.** Every spelling of the case's working directory
   is replaced by `$WORK`: the native path (`C:\Users\...`), the same with
   forward slashes (`C:/Users/...`), the MSYS form (`/c/Users/...`), the
   Cygwin form (`/cygdrive/c/...`) and the resolved path if it differs (8.3
   short names, symlinks). Matching ignores case. Cases print paths relative
   to the working directory where they can, so this rarely triggers.
3. **The shell's name at the start of a line.** A run of `NAME: ` prefixes at
   the start of any line, where `NAME` is `bash`, `wbsh`, `sh`, the basename
   of the shell under test (with or without `.exe`), `case.sh` or
   `./case.sh`, becomes a single `SHELL: `. So bash's
   `case.sh: line 3: x: unbound variable` and a wbsh message
   `wbsh: line 3: x: unbound variable` compare equal. The `line N: ` part is
   kept: bash prints it for every error in a script, and a shell that leaves it
   out differs from bash.
4. **Optional: line numbers.** Only with `--ignore-line-numbers`, a
   `line N: ` right after `SHELL: ` (or after `SHELL: builtin: `) is
   removed on both sides. Use it to see what is left once the line-number
   gap is out of the way; CI reports the strict number.

Nothing else is rewritten: no whitespace trimming, no sorting, no PID or date
masking. Cases are written so they do not need it.

## Writing a case

A case is a small script in `cases/<area>/<name>.sh`. Pick the area that
fits (`arith`, `arrays`, `brace`, `builtins`, `cmdsubst`, `control`,
`errexit`, `functions`, `glob`, `heredoc`, `paramexp`, `printf`, `quoting`,
`redirect`, `splitting`, `subshell`, `traps`) or start a new directory.

Rules, so the case means the same thing on Linux and on Windows:

- Deterministic output only: no PIDs, dates, times, `$RANDOM` without a seed,
  `uname`, user or host names, absolute paths.
- Builtins first. External programs are limited to the few both platforms
  ship identically: `cat`, `sort`, `tr`, `printf`, `mkdir`, `touch`. Use a
  small bash function instead of `grep`, `sed`, `od` or `wc`.
- Files only inside the working directory, with names Windows accepts (no
  `*`, `?`, `"`, `<`, `>`, `|`, `:` and no two names differing only by case).
  Create files with `: > name` rather than `touch` when the name starts with
  `-`.
- No sleeping and no timing. Background jobs are fine if the case `wait`s.
- To test an error message, send it to stdout with `2>&1` in the case
  itself, or let it reach stderr; both are compared.
- One behaviour per case, a few lines long. The name says what is tested.
- Case names must stay unique when compared case-insensitively.

When a case needs a newer bash than 4.4, say so in one of its first five lines:

```bash
# requires: bash>=5.3
```

The runner then counts it as `ref-too-old` when the reference bash is older,
instead of failing it.

To check a new case, record and compare it alone:

```sh
python -I tools/difftest/run.py --shell bash --record /tmp/ref --filter 'glob/my_case'
python -I tools/difftest/run.py --shell x64/Release/wbsh.exe --compare /tmp/ref --filter 'glob/my_case' --diff
```

A case wbsh fails is welcome: it is a bug report with a built-in check. Add the
case, and an entry to `FINDINGS.md` if it is a new class of problem.

## CI

`.github/workflows/ci.yml` builds GNU bash 5.3 from the release tarball on
Ubuntu, records the reference there (job `difftest-reference`), then runs the
corpus under the freshly built wbsh on Windows (job `difftest`) and writes the
summary to the job page. The job fails only when the pass count drops below
`baseline.txt`. Raise the baseline when wbsh gets better; never lower it to
make a regression pass.

## First local results (2026-11-12)

wbsh 1.1.1 (0d0f1b6 plus the uncommitted `feat/drop-in-roadmap` changes)
against Git for Windows' bash 4.4.23 on Windows 10 22H2, 440 cases:

| run | pass | stderr | fail | ref-too-old |
| --- | ---: | ---: | ---: | ---: |
| strict | 363 / 426 (85.2 %) | 4 | 59 | 14 |
| `--ignore-line-numbers` | 380 / 426 (89.2 %) | 0 | 46 | 14 |
| bash 4.4 against itself | 426 / 426 | 0 | 0 | 14 |
| strict, after the same day's backlog fixes | 420 / 426 (98.6 %) | 0 | 6 | 14 |

Caveat: bash 4.4 is not the reference CI uses. A few cases encode 4.4
behaviour that 5.x changed (for example `glob/dot_patterns`, where 5.2's
`globskipdots` makes `.*` skip `.` and `..`, as wbsh already does), and the
14 cases marked `requires: bash>=5.x` are not compared at all. The CI number
against bash 5.3 is the one to quote. The clearest real divergences are in
`FINDINGS.md`.

## Relation to `wbsh/stress`

`wbsh/stress/suites/bugs/diff` also compares wbsh with bash, but against
whatever bash is installed locally (Git Bash), as part of a stress run that
looks for crashes, hangs and regressions. This corpus is the compatibility
measure: many one-behaviour cases, a pinned reference (bash 5.3 built from
source), and a pass count that CI tracks. A case that crashes or hangs wbsh
belongs in the stress suites as well.
