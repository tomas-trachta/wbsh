# Stress-test plan for wbsh 1.0

This plan tells how we stress-test wbsh before we release the first stable
version (1.0). It has five areas: memory, speed, common bugs, security and
user experience. The tool for the automatic part is the framework in
[`wbsh/stress/`](../wbsh/stress/README.md). Use it in the development cycle
and for the release gate.

## 1. Scope

In scope:

- `wbsh.exe` as a script interpreter (`wbsh script.sh`, `wbsh -c`).
- `wbsh.exe` as an interactive shell (line editor, prompt, completion, history, jobs).
- The bundled commands (coreutils, awk, sed, grep, make, curl, tar, vi, …).
- The installer and the portable ZIP (start-up, PATH, first run).

Out of scope for 1.0: `wbshterm`, the SDK plug-in API (test it only for crashes), a Linux build.

## 2. Release criteria

wbsh 1.0 can ship when all of these are true. Measure them with
`stress.ps1 -Mode full` on two machines (one developer PC, one clean
Windows 11 VM without Git for Windows).

| # | Area | Criterion |
|---|---|---|
| R1 | Bugs | `wbsh/tests/run-all.sh` passes in golden mode (only the known machine-specific goldens fail). |
| R2 | Bugs | 0 crashes and 0 hangs in 100 000 parser mutants and 20 000 generated programs (several seeds). |
| R3 | Bugs | Every `fuzz-exec` difference from bash is triaged: fixed, or written down as a deliberate difference. |
| R4 | Bugs | Every case in `bugs/robust` and `bugs/diff` passes, or has an open issue with the 1.0 milestone removed by decision. |
| R5 | Memory | No `leak` or `handle-leak` finding. Streaming cases stay under their `maxmem` budget. |
| R6 | Memory | The ASan build runs the full test suite and the fuzz corpus without a report (see 4.1). |
| R7 | Speed | No perf case is more than 2 × slower than Git Bash. The median over all cases is at or below 1 ×. Start-up < 50 ms. |
| R8 | Speed | No regression > 25 % against the 1.0-rc baseline. |
| R9 | Security | 0 `critical` and 0 `high` security findings. Every `review` item has a written decision. |
| R10 | Security | `wbsh.exe` has ASLR (DYNAMIC_BASE, HIGH_ENTROPY_VA), DEP (NX_COMPAT) and CFG. The installer and the binaries are code-signed. |
| R11 | UX | 0 `high` UX findings. The manual checklist (section 4.5) is complete on two terminals. |
| R12 | Soak | An 8-hour soak (section 4.1) ends with flat memory and handle counts. |

## 3. Severity

| Severity | Meaning | Release |
|---|---|---|
| critical | Crash, code execution from data, a security boundary broken. | Blocks. |
| high | Hang, leak, wrong result in a common case, a missing exploit mitigation. | Blocks. |
| medium | Wrong result in a rare case, a slow path, an unclear message. | Fix or defer by decision. |
| low | Cosmetic, documentation. | Does not block. |
| review | A human must judge it. It is not a failure until somebody decides. | Needs a decision. |

## 4. Test areas

### 4.1 Memory

**Goal:** wbsh does not leak memory or handles, and memory does not follow
the size of streamed data.

**Automatic (suite `memory`):** each case repeats one operation N times
and 10 × N times. The driver compares the peak private memory of the
wbsh process and its peak handle count. A leak shows as growth per
iteration. The run saves a memory timeline (`*.timeline.csv`) for each
case. Seed cases: variables, functions, `$(...)`, subshells, native
processes, here-docs, arrays, regex and glob caches, `eval`/`source`,
file descriptors, process substitution, traps, recursion, streaming
gzip and text filters, large captures.

**Manual or occasional:**

1. **AddressSanitizer build.** Build with
   `MSBuild wbsh\wbsh.vcxproj -p:Configuration=Release -p:Platform=x64 -p:EnableASAN=true -p:OutDir=<repo>\x64\Asan\ -p:IntDir=<repo>\x64\Asan\obj\` (absolute paths, both under the ignored `x64\` folder),
   copy `clang_rt.asan_dynamic-x86_64.dll` from the MSVC tools folder beside the exe, and run
   `run-all.sh` and `stress.ps1 -Exe x64\Asan\wbsh.exe -Suite bugs,memory`.
   ASan finds use-after-free, overflows and double frees that do not crash.
2. **Page heap.** `gflags /p /enable wbsh.exe /full` (Debugging Tools for
   Windows), then run the bugs suite. It finds heap overruns in code that ASan does not instrument.
3. **Soak.** Start one interactive wbsh in Windows Terminal and one
   `crond`. Let a script run commands for 8 hours (`while :; do …; done`
   with the memory cases' bodies). Record private bytes and handles every
   minute with `Get-Process wbsh` or Performance Monitor. Both lines must be flat.
4. **Long history.** A history file with 100 000 lines: start-up time and memory.

### 4.2 Speed

**Goal:** wbsh is not slower than Git Bash for typical scripts, and it does
not become slower from release to release.

**Automatic (suite `perf`):** each case runs with warm-up runs and timed
runs. The report gives the median wall time, CPU time of the whole
process tree, peak memory, and the ratio to Git Bash on the same script.
A per-machine baseline (`baselines/<computer>.json`) catches regressions.
Seed cases: start-up, arithmetic loop, function calls, string expansion,
`$(...)`, subshells, native process start, `while read`, text pipelines,
globbing, arrays, gzip, pipe throughput, `find`.

**Manual or occasional:**

1. **Profile the slowest cases.** Use Windows Performance Recorder
   (`wpr -start CPU`, run the case, `wpr -stop out.etl`) or the Visual
   Studio profiler on the Release build with the PDB. Look at the top
   functions of the cases with the worst ratio.
2. **Cold start.** Measure the first start after a reboot and with
   Microsoft Defender real-time scanning on. Users see this number, not the warm one.
3. **Real projects.** Time `make -j` of a medium C project, `git`-heavy
   scripts (for example a `git log` / `git status` loop in a big
   repository), and an npm `postinstall` script.
4. **Slow storage.** Run the glob and `find` cases on a network share
   and on OneDrive folders.

### 4.3 Common bugs

**Goal:** wbsh gives the same result as bash for the same script, and no
input makes it crash or hang.

**Automatic (suite `bugs`):**

- `robust/`: deep nesting (5 000 parentheses, 300 nested `$(...)`,
  2 000 nested `if`), infinite recursion, 20 MB values, 200 000
  arguments, huge allocations, arithmetic overflow, closed stdin and
  stdout, NUL bytes, parallel appends to one file, 60 background jobs,
  150-stage pipelines, `timeout`, CRLF and BOM scripts.
- `diff/`: curated scripts that must print the same as bash:
  quoting and splitting, parameter expansion, arithmetic, control flow,
  redirection and here-docs, `printf`/`echo`, `read`/IFS, globbing,
  traps and exit status.
- `corpus/`: hostile parser inputs; all replay on every run.
- `fuzz-parse`: mutations of the test scripts go through the parser only
  (`wbsh --no-ast`). Nothing executes. Only crashes and hangs count.
- `fuzz-exec`: a grammar generates random but valid programs from the
  subset of bash that both shells support. Each program runs in wbsh and
  in bash in an empty folder. Differences go to triage.

**Manual or occasional:**

1. **Real-world scripts.** Run large bash scripts that people use:
   `bash-completion`, `nvm.sh`, `git`'s `t/test-lib.sh` with a few test
   files, `autoconf`-generated `configure` scripts, `rustup-init.sh`,
   `install.sh` of popular tools. Compare with Git Bash.
2. **Coverage.** Build with `/fsanitize-coverage` or use
   OpenCppCoverage on the test run. Write new `diff/` cases for the
   expander and executor branches that no test reaches.
3. **Long fuzzing.** Run `stress.ps1 -Suite bugs -FuzzCount 20000` every
   night with a new seed. Later: a libFuzzer harness for the lexer and the
   parser (clang-cl, `-fsanitize=fuzzer,address`).

### 4.4 Security

**Goal:** wbsh does not run code that the user did not ask for, and it does
not make an existing boundary weaker.

**Threat model.** These are the inputs an attacker can control:

| Input | Example | Risk |
|---|---|---|
| The current folder | A cloned repository, an unpacked ZIP | `where.bat` beside the user hijacks `where`; a hostile `.git/config` runs code through the git prompt. |
| Data | File names, `read` input, downloaded text | Data becomes code (injection through `eval`-like paths, arithmetic subscripts). |
| Arguments to programs | A file name passed to a `.bat` | cmd.exe re-parses the line (BatBadBut). |
| The environment | sudo, cron, CI, a parent process | `WBSH_*` state or `BASH_ENV` becomes code; garbage crashes the shell. |
| The network | `curl`, `wget`, `httpd`, `nc`, `ftp` | Bad TLS accepted; servers listen on all interfaces. |
| Other local users | Named pipes, shared memory, temp files | Data leaks or signals from another account. |

**Automatic (suite `security`):** current-folder hijack (`.bat`, `.cmd`,
extensionless script), `.bat` argument injection, argument round-trip to
native programs, Shellshock-style variables, startup files from the
current folder, garbage in every `WBSH_*` state variable, data that must
not run, temporary-file leftovers, the git prompt in a hostile repository,
TLS checks (with `-Net`), and `static_scan.ps1` (risky APIs, PE mitigations).

**Manual:**

1. Review each item in `logs/security-static_scan.txt` and write the decision in the PR or the issue.
2. Check the DACL of the named pipes, the FIFO pipes and the
   `Local\wbsh-signals-<pid>` objects with Sysinternals `accesschk`
   from a second, non-admin account.
3. `sudo` and `su`: test from a standard user. Check that no command line,
   environment variable or current folder from the low side changes what runs on the high side.
4. `httpd`, `nc -l`, `ftp`: check the default bind address and the firewall prompt.
5. The installer: check the folder ACLs, that PATH is not changed for
   other users, and that the uninstaller removes everything.
6. Run a secret-scanning tool and a dependency check on the repository
   (vendored `conpty.dll` / `OpenConsole.exe`: record the source and the hash).

### 4.5 User experience

**Goal:** a bash user and a Windows user can both start working without surprises.

**Automatic (suite `ux`):** error messages and exit statuses (127, 126,
2, …), `--help` for every bundled command (no hang, exit 0, a manual
entry), start-up and Tab-completion latency, Unicode text and file names,
Windows programs (`.cmd`, `.com`, PowerShell, native exit codes, both path styles).

**Manual checklist** (do it in Windows Terminal, conhost and the VS Code terminal):

- [ ] First start after install: prompt, colours, no error, `wbsh --help` text is clear.
- [ ] Ctrl+C stops a foreground native program, a builtin (`sleep`, `find /`), and a pipeline, and the prompt comes back.
- [ ] Ctrl+C at the prompt clears the line; Ctrl+D exits; `exit` with running jobs warns.
- [ ] Resize the window during a long line, in `vi`, `less` and `top`.
- [ ] Paste 1 000 lines; paste a line with Unicode and tabs.
- [ ] Czech (or another) keyboard: AltGr characters, dead keys.
- [ ] Tab completion: commands, paths with spaces, `git` branches, `C:\` paths.
- [ ] History: Ctrl+R, history across two open windows, `HISTCONTROL`.
- [ ] Long paths (> 260 characters), OneDrive folders, network shares, a drive that is not `C:`.
- [ ] A standard (non-admin) account; a machine without Git for Windows.
- [ ] Error output of a mistyped command is short and says what to do.

## 5. When to run what

| When | Command | Time |
|---|---|---|
| Before each PR (with `run-all.sh`) | `stress.ps1 -Suite bugs,security,ux` | a few minutes |
| A PR that touches the executor, expander or a hot command | `stress.ps1 -Suite perf,memory -Filter <related>` | minutes |
| Every night (when CI exists) | `stress.ps1 -Mode full -Seed <date>` | ~1 hour |
| Release candidate | `-Mode full` on two machines, ASan run, soak, manual checklists, `-Net` | 2 days |
| After the release | `stress.ps1 -Suite perf,memory -Mode full -UpdateBaseline`; commit the baseline | minutes |

## 6. Process for findings

1. The run writes `report.md`. Sort by severity.
2. For each finding, open an issue with the case name, the seed and the artifact file.
3. Make the reproducer small. Add it to the correct place: `wbsh/tests/` for behaviour,
   `stress/suites/bugs/corpus/` for parser crashes, `stress/suites/*` for the stress property.
4. Fix it. The new case must fail before the fix and pass after it.
5. A deliberate difference from bash: write a `.accept` file and record why.

## 7. First run (2026-11-11, wbsh 1.0.32, `main` at 0c505a7, quick profile)

All items below were reproduced by hand after the run. They are the
start of the 1.0 bug list.

**Blocking (critical / high):**

| # | Area | Finding | Reproducer |
|---|---|---|---|
| F1 | Security | Arguments to `.bat`/`.cmd` files are re-parsed by cmd.exe: a file name such as `file & calc.txt` runs `calc.txt]` as a second command; `x>pwned.txt` creates a file (BatBadBut class). | `security/batch_args.sh` |
| F2 | Security | The `\g` prompt runs `git status` in every repository the user enters, so a hostile `.git/config` (`core.fsmonitor`) runs a command on `cd`. | `security/git_prompt_repo.sh` |
| F3 | Memory | Every parsed text (`$(...)`, `eval`, `source`) keeps its parser arena for the life of the shell (`Executor::parseText`, `owned_arenas_`): about 17.5 kB per parse. 100 000 command substitutions in a loop keep about 1.7 GB. | `memory/cmdsubst.sh`, `memory/eval_source.sh` |
| F4 | Bugs | Function recursion deeper than about 2 000 levels ends wbsh with exit 127 and no message; bash runs 10 000 levels. | `bugs/robust/limits.sh` |
| F5 | Bugs | A syntax error such as `if then fi` gives no message and exit 0. | `ux/errors.sh` |
| F6 | Bugs | `${s/\/usr/U}`: a pattern with an escaped `/` never matches. | `bugs/diff/param_expansion.sh` |
| F7 | UX | `yes --help`, `watch --help` and `fzf --help` do not stop. | `ux/help_all.sh` |

**Medium:**

- Parallel `>>` appends from background jobs lose about 20 % of the lines and mix lines (`bugs/robust/io_edges.sh`).
- `./script.bat` and `./script.cmd` do not run: cmd.exe reads `.` as the command (`ux/native_commands.sh`).
- `chcp` (`chcp.com`) is not found: `.COM` on `PATHEXT` is not searched.
- `$SYSTEMROOT`, `$WINDIR`, `$COMSPEC`, `$PROGRAMFILES`, `$PROGRAMDATA` are empty; Git Bash scripts use these upper-case names (`ux/windows_env_names.sh`).
- Running a directory gives 127, not 126. A failed readonly assignment sets `$?` to 0. `echo` to a closed stdout returns 0.
- Many bundled commands do not know `--help` (`cat`, `ar`, `bunzip2`, …). `awk length()` counts bytes while `wc -m` counts characters.
- `IFS` is imported from the environment (bash resets it).
- Script syntax errors print `parse error 16:1: …` without `wbsh:` and the script name.
- `$(...)` keeps NUL bytes (bash drops them). wbsh accepts `[[ -x > 3 ]]` (bash: syntax error).

**Speed** (median, wbsh / Git Bash): `pipe_throughput` 5.1 × slower (200 MB: 5.0 s against 1.0 s),
`glob_many` 4.3 ×, `find_tree` 3.5 ×, `compress` 2.2 ×, `text_pipeline` 1.8 ×.
wbsh is faster in all interpreter cases: `$(...)` 17 ×, native process start 1.6 ×, `while read` 6 ×, subshells 2.8 ×.
Start-up: 24–31 ms.

**Clean:** streaming memory (gzip, text filters) stays at 1.5 MB for 10 × the data;
no handle leaks; 0 parser crashes in 300 mutants and the corpus; current-folder
hijack, Shellshock-style variables, data-as-code, temporary files and argv
round-trip all pass; `wbsh.exe` has ASLR and DEP but no CFG (`/guard:cf`).

## 8. Next steps for the framework

1. CI: build Release, run `run-all.sh` and `stress.ps1 -Suite bugs,security,ux` on every push; nightly `-Mode full`.
2. An ASan configuration in `wbsh.vcxproj`, so the ASan build is one command.
3. A minimiser for fuzz findings (delta debugging on lines, then on characters).
4. More generator grammar: `[[ ]]` regex, `printf` formats, `declare` attributes, traps, `getopts`, here-docs with expansions.
5. Interactive tests through the pseudoconsole (`script`/ConPTY): Ctrl+C, resize, prompt rendering time.
6. A real-world script corpus in `stress/suites/bugs/realworld/` (with licences checked).
7. A trend page: keep `report.json` of each nightly run and plot the perf and memory numbers.
