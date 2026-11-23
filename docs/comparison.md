# wbsh compared

wbsh against the other ways to get bash on Windows. Every cell says where it
comes from and when:

- **measured**: run by us, on the machine and date given, with the raw data
  committed in this repository;
- **documented**: taken from the project's own package or release page on
  the date given.

"Not measured" means exactly that; it is not a "no".

Machine for every measurement: Windows 10 22H2 (build 19045), AMD Ryzen 7
5800H, 16 GB (see [bench/2026-11-12-local.md](bench/2026-11-12-local.md)).

## Compatibility

| | wbsh | Git Bash | MSYS2 | WSL |
| --- | --- | --- | --- | --- |
| bash it implements | bash 5.2/5.3 behaviour, own implementation (`$BASH_VERSION` 5.2.37). Measured 2026-11-13 | GNU bash 4.4.23 in Git for Windows 2.35.1 as installed here. Measured 2026-11-13. Current Git for Windows is 2.56.1 (documented, [release page](https://github.com/git-for-windows/git/releases/latest), 2026-11-13), bundled bash version not stated there | GNU bash 5.3.20 (`bash 5.3.20-3`, documented, [packages.msys2.org](https://packages.msys2.org/packages/bash), 2026-11-13) | the distribution's GNU bash, e.g. 5.2.21 on Ubuntu 24.04 (documented, [packages.ubuntu.com](https://packages.ubuntu.com/noble/bash), 2026-11-13) |
| GNU bash 5.3 upstream suites, byte-identical | 68/86, 18 env-only, no real failures. Measured 2026-11-23, [compat-ledger.md](compat-ledger.md) | not measured (bash 4.4 predates the 5.3 suites) | not measured | not measured (GNU bash itself) |
| Differential corpus vs GNU bash | 420/426 vs bash 4.4 locally (measured 2026-11-12, [FINDINGS.md](../tools/difftest/FINDINGS.md)); CI compares against bash 5.3 with a floor of 400 ([baseline.txt](../tools/difftest/baseline.txt)) | it is the reference | GNU bash | GNU bash |
| Real-world assets sourced cleanly | 275 / 1,034 runs (26.6 %) over 7 pinned assets. Measured 2026-11-13, [corpus snapshot](corpus/2026-11-13-local.md) | 969 / 1,034 runs (93.7 %), same files. Measured 2026-11-13, [corpus snapshot](corpus/2026-11-13-local.md) | not measured | not measured |

## Speed

Same machine, same harness ([tools/bench](../tools/bench/README.md)), median
of the runs in [bench/2026-11-12-local.md](bench/2026-11-12-local.md).
MSYS2 was not installed for that run.

| | wbsh | Git Bash | MSYS2 | WSL |
| --- | --- | --- | --- | --- |
| `-c true` startup | 43 ms. Measured 2026-11-12 | 66 ms. Measured 2026-11-12 | not measured | not measured |
| 20k function calls | 196 ms. Measured 2026-11-12 | 225 ms. Measured 2026-11-12 | not measured | not measured |
| 50k arithmetic iterations | 184 ms. Measured 2026-11-12 | 321 ms. Measured 2026-11-12 | not measured | not measured |
| Process model | no `fork()`: subshells and built-ins run in-process, externals via `CreateProcess` | Cygwin-style `fork()` emulation | Cygwin-style `fork()` emulation | real Linux `fork()` in a VM |

## Footprint and distribution

| | wbsh | Git Bash | MSYS2 | WSL |
| --- | --- | --- | --- | --- |
| Install size | one 3.9 MB `wbsh.exe` (3,994,112 bytes, x64 Release, measured 2026-11-13) | 272 MB `C:\Program Files\Git` (Git for Windows 2.35.1, measured 2026-11-13) | not measured | a Linux distribution image; not measured |
| winget / scoop | manifests generated per release; scoop installs from this repository, winget pending acceptance (see README) | winget: `Git.Git` (documented) | winget: `MSYS2.MSYS2` (documented) | built into Windows (`wsl --install`) |
| Code-signed installer | no; a SignPath signing step is prepared in `release.yml` but not active ([signing.md](signing.md)) | yes (unverified) | not checked | Microsoft-signed (unverified) |
| ARM64 Windows | ARM64 installers and ZIPs are built in CI (cross-compiled, `windows-arm64` job); not yet run on ARM64 hardware | yes (unverified) | not checked | yes (unverified) |
| `Ctrl-Z` job control on Windows | yes | yes (MinTTY / ConPTY) | yes | yes |
| CI on every push | yes: build, golden suite, difftest vs bash 5.3, upstream suites, perf gate ([ci.yml](../.github/workflows/ci.yml)) | yes | yes | n/a |

Cells marked "unverified", and the Git Bash, MSYS2 and WSL packaging and job-control cells, are
general knowledge about those projects, not checked on a specific date;
treat them as context, not evidence.

## Refreshing this page

Re-run the harnesses (`tools/bench`, `tools/bashtests`, `tools/corpus`),
commit their outputs under `docs/`, and update the cells with the new date.
Another shell is measured the same way: pass its `bash.exe` as `--shell` to
the three harnesses on the same machine and commit the output beside ours.
