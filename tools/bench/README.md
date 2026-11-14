# Benchmarks

`bench.py` times a fixed set of workloads under one or more shells, in the
style of hyperfine: warm-up runs first, then N timed runs. Within each run
every case goes through every shell before the next case starts, and the
shell order rotates from run to run, so a change in machine load hits every
shell alike. Python 3.8+, stdlib only, safe under `python -I`.

```sh
python -I tools/bench/bench.py \
    --shell wbsh=x64/Release/wbsh.exe \
    --shell "git-bash=C:/Program Files/Git/bin/bash.exe" \
    --runs 30 --json bench.json --markdown bench.md
```

| option | effect |
| --- | --- |
| `--shell NAME=PATH` | a shell to time, repeatable; the first one is the baseline for the ratios |
| `--runs N` | timed runs per case and shell (default 10; the published protocol is 30) |
| `--warmup N` | untimed runs per case and shell first (default 1) |
| `--scale F` | multiply every loop count, to fit a slower or faster machine |
| `--filter GLOB` | only the matching cases, repeatable |
| `--json FILE` | machine description, protocol, statistics and every raw sample |
| `--markdown FILE` | the report table, also printed to stdout |
| `--low-priority` | run the harness and every shell below normal priority |
| `--list` | list the cases |
| `--yardstick`, `--gate`, `--write-baseline`, ... | the perf gate, see below |

Each cell is the wall time from process start to exit, reported as the
median (p50) with the 90th percentile; the JSON also has min, mean and
standard deviation. A run that exits non-zero is counted as a failure and
left out of the statistics.

## Cases

| case | workload |
| --- | --- |
| `startup` | `<shell> -c true` |
| `function_calls` | 20000 calls of an empty function |
| `arith_loop` | 50000 iterations of `(( x += i ))` |
| `string_append` | 20000 appends of one character |
| `array_fill` | 20000 appends to an indexed array |
| `builtin_pipeline` | 25 runs of `echo \| tr \| cut` |
| `external_spawn` | 150 runs of `%SystemRoot%\System32\hostname.exe` |
| `cmdsubst_loop` | 100 command substitutions `$(echo $i)` |
| `read_loop` | `while read` over a 5000-line file the harness writes first |

Every case except `startup` is a script file run as `<shell> <case>.sh`, so
parsing the script is part of the measurement.

`external_spawn` needs a native program that every Windows machine has, that
takes no arguments (MSYS shells rewrite arguments such as `/c` into paths),
and that does almost nothing. `hostname.exe` fits: it prints one line and
exits. It is called by its full forward-slash path so that no shell can
resolve it to a different `hostname` (Git Bash ships its own in `/usr/bin`)
and PATH lookup is not part of the cost.

## Publishing results

Run on an otherwise idle machine, with `--runs 30`, and commit the markdown
under `docs/bench/<date>-<machine>.md` together with the machine description
(Windows build, CPU, memory) and the exact command line. Keep the JSON beside
it when the raw samples matter.

`wbsh/stress/` has a perf suite too; that one guards wbsh against its own
baseline (regressions, memory, ratio limits), while this harness produces the
cross-shell table for the README.

## Perf gate

The gate fails a run when any case got more than 20 % slower than a stored
baseline. Absolute times are useless for that in CI: GitHub's Windows
runners differ from each other and from a desktop by far more than 20 %, and
from one week to the next. So the gate never compares milliseconds across
machines. It compares **ratios measured in the same run**:

1. Every gated run times wbsh and a *yardstick* shell, the machine's own
   Git Bash, interleaved case by case as usual. Load spikes and a slow or
   fast runner hit both shells alike.
2. For each case the metric is `p50(wbsh) / p50(git-bash)`.
3. The baseline stores that ratio per case, together with the yardstick's
   `$BASH_VERSION`, the machine and the protocol
   (`tools/bench/baselines/<runner>.json`, one file per machine class:
   `local.json` for the maintainer's desktop, `github-windows-2022.json` for
   CI's runner image).
4. A case regresses when its ratio is more than `--threshold` (0.20) above
   the baseline ratio **and** wbsh is at least `--min-delta-ms` (3 ms)
   slower than the baseline ratio predicts. The second condition keeps the
   millisecond-scale `startup` case from flapping on scheduler noise.
5. Cases that look regressed are measured once more (same protocol) and the
   better ratio of the two runs is judged, so a single noisy run does not
   fail CI.
6. If the yardstick's version differs from the one in the baseline (the
   runner image updated Git for Windows), the table is still printed but the
   gate is not enforced and asks for a new baseline: a faster Git Bash
   would otherwise look like a wbsh regression.

```sh
# record a baseline for this machine
python -I tools/bench/bench.py --shell wbsh=x64/Release/wbsh.exe \
    --shell "git-bash=C:/Program Files/Git/bin/bash.exe" --yardstick git-bash \
    --runs 15 --low-priority --write-baseline tools/bench/baselines/local.json

# gate a build against it
python -I tools/bench/bench.py --shell wbsh=x64/Release/wbsh.exe \
    --shell "git-bash=C:/Program Files/Git/bin/bash.exe" --yardstick git-bash \
    --runs 15 --low-priority --gate tools/bench/baselines/local.json

# judge an earlier --json run without measuring again
python -I tools/bench/bench.py --from-json bench.json --yardstick git-bash \
    --gate tools/bench/baselines/local.json
```

| option | effect |
| --- | --- |
| `--yardstick NAME` | divide every case by this shell's p50; omit it to compare raw seconds (same machine only) |
| `--write-baseline FILE` | store this run's per-case metric as a baseline |
| `--gate FILE` | compare with FILE; exit 1 on a regression |
| `--threshold F` | relative slowdown that fails (default 0.20) |
| `--min-delta-ms MS` | absolute slowdown below which a case never fails (default 3) |
| `--from-json FILE` | judge a saved `--json` run instead of measuring |
| `--summary-md FILE` | append the gate table (CI passes `$GITHUB_STEP_SUMMARY`) |

In CI the `bench` job in `.github/workflows/ci.yml` runs 10 interleaved
runs per cell against `wbsh.exe` from the build job. It always uploads
`bench-baseline-candidate.json`. While
`tools/bench/baselines/github-windows-2022.json` does not exist the job only
records; to arm the gate, take the candidate from a green `main` run and
commit it under that name. Re-record it deliberately when a change is meant
to trade speed for something else, and say so in the commit.

Why not compare against the previous release binary in the same run? That
is the cleanest A/B, but it needs a released `wbsh.exe` to download on every
run and moves its reference only at release time. The Git Bash ratio needs
nothing beyond the runner image, catches a regression on the commit that
introduces it, and the version guard covers the one way the yardstick can
move.
