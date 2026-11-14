# GNU bash upstream test suites

`run.py` runs the test suites that ship with GNU bash 5.3 (`tests/run-*` in
the bash source tree) under a shell and compares each suite's output with
its `.right` file byte for byte. The results feed
[`docs/compat-ledger.md`](../../docs/compat-ledger.md).

The bash sources are not vendored. `--fetch DIR` downloads
`bash-5.3.tar.gz` from ftp.gnu.org into `DIR`, checks its SHA-256
(`0d5cd869...0f31ba`, the same pin as the difftest reference build in CI),
and extracts only `bash-5.3/tests/`. A later run with the same `DIR`
reuses it, so CI can cache that directory. Nothing from the tarball is
built; its configure scripts and Makefiles are never run.

```sh
python -I tools/bashtests/run.py --shell x64/Release/wbsh.exe --fetch C:/cache/bash-src \
    --json bashtests.json --markdown bashtests-table.md --diff-dir bashtests-diffs
python -I tools/bashtests/run.py --shell x64/Release/wbsh.exe --tests C:/src/bash-5.3/tests \
    --filter 'arith*' --keep --work-root C:/scratch/bt
```

| option | effect |
| --- | --- |
| `--shell PATH` | the shell under test |
| `--fetch DIR` / `--tests DIR` | download + verify + extract into DIR, or use an extracted `tests/` |
| `--filter GLOB` | only suites whose name (`arith`, `nquote4`, ...) matches; repeatable |
| `--timeout S` | seconds per suite before the whole process tree is killed (default 180) |
| `--jobs N` | suites run in parallel (default 1; keep it at 2 or below) |
| `--json FILE` | per-suite results, categories and the env rules that applied |
| `--markdown FILE` | the per-suite table for the ledger |
| `--summary-md FILE` | append totals and the table (CI passes `$GITHUB_STEP_SUMMARY`) |
| `--diff-dir DIR` | `DIR/<suite>.diff` for every suite that is not identical |
| `--baseline FILE` | exit 1 if fewer suites are identical than the number in FILE |
| `--rules FILE` | env-only rules (default `env_rules.json`) |
| `--keep`, `--work-root DIR` | keep the per-suite sandboxes for inspection |
| `--low-priority` | run the runner and every shell below normal priority |

## How a suite runs

The runner reproduces what `make tests` does, without a build tree:

1. The shell is copied to `<work>/bin/bash.exe`, and `THIS_SH` is
   `<work>/bin/bash`. Error messages that name the shell then read
   `bash: ...`, as they do in the `.right` files, if the shell names itself
   after `argv[0]` the way bash does.
2. bash's test helpers `recho`, `zecho` and `xcase` are C programs in
   `support/`; the runner writes small `#!/bin/sh` equivalents into
   `<work>/bin` (see `HELPER_SCRIPTS` in `run.py`). `printenv` is left to
   the shell or the system.
3. Every suite gets a fresh copy of `tests/` with `HOME` and `TMPDIR`
   inside it, stdin closed, `LANG=C`, `TZ=UTC`, `TERM=dumb`, every `LC_*`
   and `WBSH_*` variable removed (so no `WBSH_INIT_COMMAND` runs) and
   `PATH` set to `<work>/bin`, `.` and the system directories.
4. The `run-*` script is executed by the shell under test. Two edits are
   made to the copy first: the `&& rm -f $BASH_TSTOUT` after its `diff`
   is dropped, so the output survives, and the two `dbg-support` suites
   write to `$BASH_TSTOUT` instead of a fixed `/tmp` path.
5. The runner compares that output file with the `.right` file itself;
   the `diff` the script prints is not trusted.

`run-all`, `run-minimal` and `run-gprof` are drivers, not suites. bash 5.3
has 86 suites; older bash releases had fewer.

## Classification

| category | meaning |
| --- | --- |
| `identical` | the output file equals the `.right` file byte for byte |
| `env-only` | every changed line (on either side) matches an env rule, or the suite is listed under `suite_rules` |
| `fail` | anything else: a real difference |
| `timeout` | the suite did not finish within `--timeout`; counted as a real failure |
| `error` | the suite could not be judged (no `.right` file found) |

Env rules live in [`env_rules.json`](env_rules.json); each has an id, a
reason and a regular expression. A suite is env-only only if **every**
added and removed line matches one of them; one unexplained line makes it a
real failure. The rules cover things Windows cannot provide whatever the
shell does:

- `umask-inherited`: the umask comes from the parent process.
- `missing-locale`: the suites' own warnings that a POSIX locale is missing.
- `windows-reserved-filename`: `:`, `*`, `?`, `"`, `<`, `>`, `|` cannot appear in file names.
- `linux-signal-list`: Linux-only signals in `kill -l` / `trap -l` output.

`suite_rules` (empty today) marks a whole suite env-only after a person has
read its diff and found nothing but environment differences; each entry
needs a reason, and it should be revisited whenever the shell changes.
Line numbers, message wording and `wbsh:` instead of `bash:` are never
env-only: a drop-in has to match them.

## CI

The `bashtests` job in `.github/workflows/ci.yml` runs every suite against
the `wbsh.exe` the `windows` job built, caches the extracted tarball, writes
the totals to the job summary and uploads the JSON and the diffs. It does
not gate yet; once the count stabilises, put it in a `baseline.txt` here and
pass `--baseline`.
