# wbsh stress tests

This folder holds the stress-test framework for wbsh. It measures speed and
memory, looks for crashes, hangs and wrong output, checks security
properties, and checks the user experience. The release plan that uses it is
in [`docs/stress-testing.md`](../../docs/stress-testing.md).

The functional test suite (`wbsh/tests/run-all.sh`) answers "is the output
correct?". This framework answers "does wbsh stay fast, small, stable and
safe under load and under bad input?".

## Run it

You need PowerShell 7 and a built `wbsh.exe`. Git for Windows is optional:
its bash is the reference shell for speed ratios and output comparisons.

```powershell
.\wbsh\stress\stress.ps1                        # quick profile, all suites (some minutes)
.\wbsh\stress\stress.ps1 -Mode full             # release gate: more runs, larger data, more fuzzing
.\wbsh\stress\stress.ps1 -Suite perf,memory     # only some suites
.\wbsh\stress\stress.ps1 -Suite perf -Filter 'read*'
.\wbsh\stress\stress.ps1 -Suite bugs -FuzzCount 5000 -Seed 42
.\wbsh\stress\stress.ps1 -Suite security -Net   # also the checks that need the internet
.\wbsh\stress\stress.ps1 -Exe x64\Debug\wbsh.exe
.\wbsh\stress\stress.ps1 -List                  # show the cases
```

| Parameter | Use |
|---|---|
| `-Mode quick\|full` | Profile. `quick` is for the development cycle. `full` is for a release. |
| `-Suite` | Any of `perf`, `memory`, `bugs`, `security`, `ux`. |
| `-Filter` | Wildcard on case names. |
| `-Exe` | The wbsh.exe to test. Default: `x64\Release`, then `x64\Debug`. |
| `-RefShell`, `-NoRef` | The reference bash, or no reference. |
| `-FuzzCount`, `-Seed` | Number of generated programs, and the random seed (printed on every run, so you can repeat a run). |
| `-Jobs` | Parallel processes for fuzzing. |
| `-UpdateBaseline` | Write the perf and memory numbers of this run to `baselines/<computer>.json`. |
| `-KeepWork` | Keep the work folders of passed cases. |
| `-Net` | Run the checks that need the internet. |

**ASan:** when the folder of `-Exe` holds `clang_rt.asan_dynamic-x86_64.dll`,
the driver sets `ASAN_OPTIONS` so that every wbsh process writes its
AddressSanitizer report to `results/<run>/asan/`. Each report is a
`critical` finding. See `docs/stress-testing.md` § 4.1 for the build command.

The exit code is 0 when no case failed and nothing regressed. `review`
findings do not change the exit code.

Each run writes `results/<timestamp>/`:

- `report.md` and `report.json`: the summary tables and all findings.
- `logs/`: the output of each case, memory timelines (`*.timeline.csv`), static-scan details.
- `fuzz-parse/`, `fuzz-exec/`: the inputs that crashed, hung or printed other output than bash.

## How it measures

`lib/ProcessProbe.cs` starts each case suspended inside a Windows Job object,
then resumes it. Thus the numbers include wbsh **and all its child
processes**:

- wall time, CPU time (user + kernel, whole tree), number of processes;
- peak committed memory of the tree and of the root process;
- peak handle count of the root process, sampled every 20 ms;
- crash (NTSTATUS exit code such as `0xC0000005`), timeout (the whole tree is killed), and processes left alive after wbsh exits.

## Suites

| Suite | Folder | What a case is | Pass rule |
|---|---|---|---|
| perf | `suites/perf/*.sh` | A workload. An optional `<name>.setup.sh` makes its data first (not timed). | Exit 0. No baseline regression. Optional `maxratio` (vs bash) and `maxmem`. |
| memory | `suites/memory/*.sh` | One operation repeated `$STRESS_ITERS` times. | Run at N and 10×N: root peak memory must not grow more than `tolerance`; handle count must not grow more than `handles`. |
| bugs | `suites/bugs/robust/*.sh` | Assertions on edge cases (deep nesting, huge data, closed descriptors). | No `not ok` line, no crash, no hang. |
| | `suites/bugs/diff/*.sh` | A script that must give the same output as bash. | Same stdout+stderr and exit code as bash, or the same text as `<name>.accept`. |
| | `suites/bugs/corpus/*` | Hostile parser inputs. | `wbsh --no-ast` does not crash or hang. |
| | (generated) | `fuzz-parse`: mutated scripts through the parser only. `fuzz-exec`: random well-formed programs, run in wbsh and bash. | No crash, no hang. Output differences are `review` findings. |
| security | `suites/security/*.sh`, `*.ps1` | Assertions on security properties; `static_scan.ps1` reviews the sources and the PE header. | No `not ok` line. |
| ux | `suites/ux/*.sh` | Assertions on messages, exit codes, help, start-up time, Unicode, Windows programs. | No `not ok` line. |

## Add a case

1. Put a `.sh` file in the correct suite folder. One file is one case.
2. Add `# @stress key=value ...` in the first 40 lines if you need other settings:

   | Key | Suites | Default | Meaning |
   |---|---|---|---|
   | `timeout` | all | 180 (perf 300, memory 600) | Seconds before the tree is killed. |
   | `runs`, `warmup` | perf | profile | Timed runs and untimed warm-up runs. |
   | `ref` | perf | `bash` | `none` turns off the bash comparison. |
   | `maxratio` | perf | none | Fail when wbsh is more than this many times slower than bash. |
   | `maxmem` | perf, memory | none | Fail above this peak (MB) of the process tree. |
   | `tolerance` | perf, memory | 0.25 / 4 | perf: allowed slowdown vs baseline (0.25 = 25 %). memory: allowed growth in MB. |
   | `n`, `factor` | memory | 500, 10 | Small iteration count, and the multiplier for the large run. |
   | `handles` | memory | 64 | Allowed growth of the peak handle count. |

3. For assertion cases (bugs/robust, security, ux), source the helpers and end with `finish`:

   ```bash
   . "$STRESS_ROOT/lib/check.sh"
   severity high                         # critical | high | medium | low
   wbsh_run 'cd /no/such/dir'            # fresh child wbsh: RUN_OUT, RUN_ERR, RUN_RC
   expect_eq "cd fails with 1" "$RUN_RC" 1
   check "file exists" [ -f out.txt ]
   review "a finding a human must judge"
   finish
   ```

   Run risky input in a child wbsh (`wbsh_run` or `"$STRESS_WBSH"`), so a crash becomes a failed check and not a lost case.

4. A `.ps1` file in `security/` or `ux/` is a PowerShell case. It gets `-Context @{ Repo; Wbsh; OutDir }` and returns objects with `Severity`, `Text` and `Artifact`.

Every case gets these variables: `STRESS_WBSH` (wbsh.exe under test), `STRESS_TMP` (its own empty work folder, also the current folder), `STRESS_ROOT` (this folder), `STRESS_REPO`, `STRESS_N` (profile scale: 1 quick, 3 full), `STRESS_MODE`, `STRESS_SHELL` (`wbsh` or `bash`), `STRESS_NET`, and for memory cases `STRESS_ITERS`.

## From a finding to a fix

1. A **crash or hang from fuzzing**: the input is in `results/<run>/fuzz-*/`. Make it small, fix wbsh, then copy the small input to `suites/bugs/corpus/` (parser) or write a `bugs/diff/` or `wbsh/tests/` case (behaviour). The corpus replays on every run.
2. A **difference from bash**: decide if it is a wbsh bug or a deliberate difference. A bug: fix it and add a test. A deliberate difference: write the wbsh output to `suites/bugs/diff/<name>.accept` and give the reason in the case header.
3. A **leak**: open `logs/memory-<case>.timeline.csv`. A line that rises the full run is a leak; a step that stays flat is a cache.
4. A **perf regression**: run the case again with `-Mode full -Filter <case>` to remove noise. When a change makes a case faster or slower on purpose, run `-UpdateBaseline` and commit the new baseline file in the same PR.
