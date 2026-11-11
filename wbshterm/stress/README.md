# wbshterm stress tests

This folder holds the stress-test framework for wbshterm. It uses the same
probe, findings and report format as the wbsh framework
([`wbsh/stress/`](../../wbsh/stress/README.md)); the shared parts are in
[`tools/stress/`](../../tools/stress). The release plan is in
[`docs/stress-testing.md`](../../docs/stress-testing.md).

`tests/smoke.ps1` answers "do the headless modes work?". This framework
answers "does the terminal stay fast, small, correct and safe under load and
under hostile output?".

## Run it

You need PowerShell 7 and a built `wbshterm.exe` with `wbsh.exe` beside it
(`msbuild wbsh.sln /t:wbshterm /p:Configuration=Release /p:Platform=x64`).

```powershell
.\wbshterm\stress\stress.ps1                         # quick profile, all suites (about 5 minutes)
.\wbshterm\stress\stress.ps1 -Mode full              # release gate
.\wbshterm\stress\stress.ps1 -Suite bugs -Filter 'vt*'
.\wbshterm\stress\stress.ps1 -Suite bugs -FuzzCount 5000 -Seed 9
.\wbshterm\stress\stress.ps1 -Suite memory -Gui      # also the window soak (opens a real window)
.\wbshterm\stress\stress.ps1 -Suite bugs -Record     # write .expect files that are missing
.\wbshterm\stress\stress.ps1 -List
```

| Parameter | Use |
|---|---|
| `-Mode quick\|full` | Profile: runs, data scale, fuzz count, window soak time. |
| `-Suite`, `-Filter` | Suites (`perf`, `memory`, `bugs`, `security`, `ux`) and a wildcard on case names. |
| `-Exe`, `-Shell` | The wbshterm.exe to test, and the shell it hosts (default: `wbsh.exe` beside it). |
| `-FuzzCount`, `-Seed`, `-Jobs` | VT fuzzing: number of streams, random seed, parallel processes. |
| `-Record` | Write the `.expect` file of a golden case from the current output. Check every line by hand before you commit it. |
| `-Gui` | Run the `*.window.ps1` soak cases, which open the real window. |
| `-UpdateBaseline`, `-KeepWork` | As in the wbsh framework. |

An ASan build is detected the same way as for wbsh (`clang_rt.asan*.dll` beside the exe).

## How it drives wbshterm

All cases use the headless modes, so they run without a window:

- **Replay** (`--replay in.raw --dump grid.txt --size CxR`): bytes in, grid text
  out, no shell. It tests the VT parser and the grid alone, it is
  deterministic, and it is fast. Memory checks subtract the input, because
  replay keeps the whole file in memory.
- **Session** (`--snapshot … --record … --feed …`): a real pseudoconsole
  with `wbsh` in it. The driver types the feeds, waits until the output is
  quiet, then replays the recording to get the final grid as text. Sessions
  get their own `HOME`, and the probe drops inherited `WBSH_*` variables, so
  the user's `.wbshrc` and start panel stay out.
- **Self-test** (`--selftest`): the built-in checks; each `FAIL` line is a finding.
- **Window** (`-Gui` only): the real window on a script, sampled for memory,
  handles, GDI and USER objects.

## Suites and case types

| Suite | Case files | Pass rule |
|---|---|---|
| perf | `*.vt.ps1` stream generators, `*.session.ps1` sessions | No baseline regression; optional `maxms`. The report gives MB/s for streams. |
| memory | `*.vt.ps1`, `*.session.ps1` at scale N and 10×N; `*.window.ps1` with `-Gui` | Growth ≤ max(`tolerance` MB, `ratio` × input growth); handles ≤ `handles`. Window: flat memory, handles, GDI, USER. |
| bugs | built-in self-test | No `FAIL` line. |
| | `vt/<name>.vt` + `<name>.expect` | The replayed grid equals the expected text exactly. |
| | `robust/*.vt.ps1` | No crash or hang; inside `maxms` and `maxmem`. |
| | `sessions/*.session.ps1` | `Expect` / `Absent` regexes and `Check` items hold. |
| | `corpus/*` | Raw hostile bytes at three grid sizes: no crash or hang. |
| | (generated) `fuzz-vt` | Random and mutated VT streams at random grid sizes: no crash or hang. |
| security | `*.session.ps1`, `static_scan.ps1` | No failed item. |
| ux | `*.ps1` (`cli.ps1`, `config_fuzz.ps1`), `*.session.ps1` | No failed item. |

## Add a case

**A golden grid** (`suites/bugs/vt/<name>.vt`): the first lines are comments;
`# @stress size=20x5` sets the grid. The other lines are joined with nothing
between them, in this notation: `\e` ESC, `\a` BEL, `\b`, `\t`, `\r`, `\n`,
`\\`, `\xHH` (one raw byte), `\u{HHHH}` (one code point). Write the
`<name>.expect` file **by hand** from the VT rules: one line per grid row,
trailing blanks cut, every row present. A golden written by `-Record` only
records what wbshterm does now; check it before you trust it.

```
# DECSC / DECRC
# @stress size=4x3
ab\e7\e[3;3Hx\e8c
```

**A stream generator** (`*.vt.ps1`): a script with `param([int]$Scale = 1)`
that returns a string (real ESC characters) or a `byte[]` (return it as
`,$bytes`). `lib/Vt.ps1` has `Csi`, `Osc` and the fuzz generators. Keys in
`# @stress`: `size`, `timeout`, `maxms`, `maxmem`, `runs`, `warmup`,
`tolerance`, `ratio`, `n`, `factor`, `handles`.

**A session** (`*.session.ps1`): a script with
`param([string]$WorkDir, [int]$Scale = 1)` that may create files in
`$WorkDir` (the session's current folder) and returns a hashtable:

| Key | Meaning |
|---|---|
| `Feed` | Strings typed one after another, in `--feed` notation: `\r` Enter, `\e` Escape, `\t` Tab, `\xHH` any byte. |
| `Size`, `Delay`, `Gap`, `Settle`, `Timeout` | Grid size; ms before the first feed, between feeds, and of quiet output before the end; seconds before the tree is killed. |
| `Shell`, `Args` | Another program to host. |
| `Expect`, `Absent`, `Severity` | Regexes the final grid must / must not match, and the severity of a miss. |
| `Check` | `{ param($r) ... }` returning `@{ Ok; What; Severity }` items. `$r` has `Dump`, `Work`, `Probe`, `Started`, `Raw`. |

**A PowerShell check** (`security/*.ps1`, `ux/*.ps1`): a script with
`param($Context)` (`Repo`, `Terminal`, `Shell`, `OutDir`, `Work`) that returns
objects with `Ok`, `Severity`, `Text` and `Artifact`. It can call
`Invoke-Probe` from `tools/stress/Core.ps1`.

## From a finding to a fix

1. **A fuzz crash or hang**: the input is in `results/<run>/fuzz-vt/`; its
   grid size is in the file name. Replay it with `wbshterm --replay <file>
   --size <size>` under a debugger, fix it, and copy the input to
   `suites/bugs/corpus/`.
2. **A golden mismatch**: the diff is in `logs/`. Decide which side is right
   with the VT specification (xterm's ctlseqs, the DEC VT510 manual). A wrong
   `.expect` file is a test bug; fix it in the same PR.
3. **A session failure**: `logs/<suite>-<case>.grid.txt` is the final grid;
   `work/<suite>-<case>/session.raw` (with `-KeepWork`) is every byte, and
   `snapshot.png` is the painted screen.
