# Contributing to wbsh

Thanks for being here. wbsh is in alpha; the surface area is large and almost
every part has room for improvement, so contributions are genuinely welcome.
This document is the map: how the codebase fits together, the conventions
that keep it readable, and what a PR needs to look like to land.

If anything here is unclear or out of date, that itself is a fixable bug —
open an issue or a PR against this file.

---

## Ground rules

- One feature or fix per PR. Bundle test updates with the change they cover.
- Match the existing style. Tabs (4-column) in `src/`. `camelCase` functions,
  `PascalCase` types, everything under `namespace wbsh`.
- Don't introduce anonymous namespaces or bare `{ ... }` scope blocks. Use
  `static` for file-local free functions, or a named sub-namespace (e.g.
  `namespace wbsh::detail { ... }`) for grouped helpers.
- Keep functions short. Soft target ≤60 lines, ≤4 levels of nesting; if you
  blow past that, extract a named helper.
- No commented-out code. Delete it; git remembers.

The longer-form C++ conventions wbsh uses are an adaptation of
[`coding_standards/c_coding_standards.md`][cs] — tabs, `#pragma once`,
`camelCase`. One deliberate deviation: wbsh reports errors as values
(status returns, error flags, `std::error_code`), not exceptions — see
the "Errors" bullet in the style cheatsheet. Project conventions win
over the standard when they conflict.

[cs]: https://github.com/trachtot/coding_standards

---

## Architecture in 5 minutes

wbsh is a real shell pipeline: source text → tokens → AST → expansion →
execution. Each stage lives in its own translation unit and is exercised
end-to-end by the test suite. Read the files in this order if you want to
get the shape of the codebase:

| Stage | File(s) | What it does |
|---|---|---|
| Lex | `src/lexer.cpp/h` | POSIX shell tokenizer. Handles quoting, here-docs, `$(...)`, balanced-paren scanning. Outputs `Token` + structured `WordSegment` lists. |
| Parse | `src/parser.cpp/h`, `src/ast.h`, `src/arena.h` | Recursive-descent parser. Produces a `Node` AST with `Kind`-tagged variants (pipeline, if, for, function, simple command, …). Nodes are bump-allocated from the parser's `Arena` and linked with raw borrow pointers; whoever needs the AST to outlive the parser takes the arena (`Parser::takeArena`, `Executor::adoptArena`). |
| Expand | `src/expander.cpp/h` | Parameter / arithmetic / command / glob / brace / tilde expansion. Calls back into the executor via `CommandSubstitutor` for `$(...)`. |
| Execute | `src/executor.cpp/h` | Walks the AST. Owns the runtime registries (builtins, functions, aliases, jobs, traps, dirstack, completion specs). Spawns external processes via Win32 `CreateProcess`. Control flow (`break`, `continue`, `return`, `exit`) propagates as a value: a pending `FlowSignal` on the Executor that every frame checks after running a child and the owning frame consumes. |
| Env | `src/environment.cpp/h` | Variables, exports, scopes, array and assoc-array storage. |
| Paths | `src/pathconv.cpp/h` | `/c/Users/...` ↔ `C:\Users\...` translation; applied at the spawn boundary when invoking native Windows `.exe`. |
| Builtins | `src/builtins.cpp` | Shell builtins (`cd`, `export`, `declare`, `read`, `trap`, `getopts`, `complete`, …). |
| Coreutils | `src/coreutils.cpp` + `coreutils_*.cpp` | Bundled `ls`/`grep`/`sed`/`awk`/`tar`/… Split per family; see "The coreutils split pattern" below. |
| Awk | `src/awk.cpp/h` | Self-contained awk implementation invoked by the `awk` coreutil. |
| Inflate | `src/inflate.cpp/h` | gzip / zip decoder used by `gunzip`, `zcat`, `unzip`, `tar -xz`. |
| REPL | `src/main.cpp`, `src/repl.cpp`, `src/lineedit.cpp` | CLI dispatch, console mode setup, prompt expansion, line editor with history + completion. |
| Setup | `src/setup.cpp` | One-time runtime bootstrap: PATH discovery, locating `git`, console / VT mode. |
| Debug | `src/printer.cpp` | Token / AST pretty-printer for the `-t` and default (no-`-r`) dump modes. |

Two cross-cutting interfaces are worth knowing about up-front:

- **`Executor` is the runtime root.** Every builtin, every coreutil, every
  completion spec hangs off it. New builtins register themselves through
  `Executor::registerBuiltin(name, fn)`. A `BuiltinFn` is a plain function
  pointer: `int (*)(Executor&, const std::vector<std::string>& argv)`.
- **`CommandSubstitutor` decouples expander from executor.** The Expander
  needs to run `$(...)` bodies, but doesn't want to depend on the full
  Executor surface. It calls `CommandSubstitutor::run(body)`; the Executor
  implements that interface.

---

## The coreutils split pattern

`coreutils.cpp` is the dumping ground for bundled utilities and tends to get
big. The pattern for splitting a related group off into its own file is
already established (see `coreutils_bc.cpp`, `coreutils_curl.cpp`,
`coreutils_hash.cpp`) and you should follow it when adding a new group:

1. New file `src/coreutils_<group>.cpp`. Put its file-local helpers in a
   named sub-namespace: `namespace wbsh::<group>_detail { ... }`. No
   anonymous namespaces.
2. Add one public entry point: `void register<Group>Builtin(Executor&);`
   (or plural if it wires multiple names). Declare it in
   `src/coreutils_internal.h` so `registerCoreutils()` can call it.
3. Call your `register…` function from `registerCoreutils()` in
   `coreutils.cpp` and delete the moved code from there.
4. Add the new file to `wbsh.vcxproj` (`<ClCompile Include="src/coreutils_<group>.cpp" />`).
5. Run the full test suite in golden mode before opening the PR.

Documented future splits, in roughly decreasing payoff:

- `coreutils_text.cpp` — `grep`, `find`, `sed`, `sort`, `uniq`, `tr`, `cut`,
  `paste`, etc. (~1200 lines; the biggest split available).
- `coreutils_archive.cpp` — `tar`, `gzip`, `gunzip`, `zcat`, `zip`, `unzip`
  (~700 lines; all share `readAllBytesFromFile` and `crc32Update`).
- `coreutils_encoding.cpp` — `base64`, `xxd`, `od` (~280 lines).

The same pattern can be applied to `executor.cpp` once it grows further —
the natural split is `executor_redirection.cpp` (fd plumbing) and
`executor_process.cpp` (Win32-specific spawn helpers).

---

## Build

```powershell
& "$env:ProgramFiles\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" `
    .\wbsh.vcxproj -p:Configuration=Release -p:Platform=x64
```

`Debug` works the same way but produces a non-redistributable binary against
the `/MDd` debug runtime. For a release-style PR check, always build
`Release|x64`.

The full release pipeline (binary + portable ZIP + Inno Setup installer):

```powershell
.\installer\build.ps1                  # default version
.\installer\build.ps1 -Version 1.0.6   # explicit
```

You only need to run `build.ps1` when touching the installer, the bundled
runtime DLLs, or the version banner. For day-to-day source edits the raw
MSBuild call is enough.

---

## Tests

The test harness is `tests/run-all.sh`. Each `tests/*.sh` script is a
hand-written smoke check exercising one slice of behavior — pipelines,
redirection, expansion, control flow, a specific builtin, a specific
coreutil. They run *inside wbsh itself*; if your change breaks the lexer or
the executor badly enough that the harness can't bootstrap, you'll see it.

Three modes:

```sh
# Exit-status mode — assert each script exits 0.
../x64/Release/wbsh.exe -r run-all.sh

# Golden mode — additionally diff combined stdout+stderr against
# tests/expected/<name>.out. This is the bar a PR has to clear.
WBSH_GOLDEN=1 ../x64/Release/wbsh.exe -r run-all.sh

# Record mode — (re)write the expected/ goldens from current output.
# Only run this when you trust the new output and have inspected the diff.
WBSH_RECORD=1 ../x64/Release/wbsh.exe -r run-all.sh
```

Some tests are non-deterministic by nature (dates, hostnames, mounted-disk
numbers, raw native-cmd output) and are listed in the `NO_GOLDEN` variable
at the top of `run-all.sh`. Add to that list rather than fighting flakes.

**When to add a test:**

- New builtin or coreutil → at least one `tests/<name>.sh` covering the
  golden path and one or two edge cases.
- New expansion form / lexer construct → a script that exercises it both
  quoted and unquoted, with at least one whitespace edge case.
- Bug fix → a script that reproduces the bug; it should fail on `main` and
  pass on your branch. (You don't need to commit a failing baseline — just
  make sure your test actually exercises the path.)

Run `WBSH_GOLDEN=1` before opening the PR. If you intentionally changed
output, run `WBSH_RECORD=1`, inspect the diff against the prior goldens,
and commit the updated `expected/*.out` files in the same PR.

There is currently no CI. Until that lands, your local run is the only
gate — so run it.

---

## Style cheatsheet

The full standard is `coding_standards/c_coding_standards.md`, adapted for
C++. The points you'll bump into most often:

- **Includes.** Source file's own header first, blank line, then groups
  (system `<...>` headers, then project `"..."` headers). Alphabetical
  inside each group.
- **Headers.** `#pragma once` (wbsh-wide convention; overrides the standard's
  `#ifndef` guards rule). Self-contained: include what you use.
- **Functions.** ≤60 lines soft, ≤4 nesting levels, ≤5 parameters; bundle
  into a struct if you need more. If a function reads like a long script,
  extract intent-named helpers — see `executor.cpp` / `expander.cpp` for
  the orchestrator+helpers pattern.
- **Switches.** Every `case` ends with `break` / `return` / a `/* fallthrough */`
  comment; always include a `default`.
- **Declarations.** Declare variables at first use, one per line, `const`
  what doesn't change.
- **Comments.** Doxygen-style `/** ... */` on every public function and
  type (we publish generated API docs from `docs/Doxyfile`). Don't restate
  what the code obviously says; comment the *why* — invariants,
  workarounds, hidden constraints.
- **No "what" comments.** `i++;  // increment i` is noise. `// MSVC's
  isspace asserts on negative chars, so cast to unsigned char first` is a
  comment.
- **Memory.** Prefer RAII / `std::unique_ptr` / `std::vector` over manual
  `new`/`delete`. The codebase has effectively no raw owning pointers
  outside Win32 handle wrappers. Node-heavy trees (the shell AST, awk's
  Expr/Stmt trees) are bump-allocated from an `Arena` (`src/arena.h`):
  nodes hold raw borrow pointers to each other and the arena owns them
  all — keep the arena alive, not the individual nodes. Don't
  heap-allocate AST nodes individually.
- **Errors.** wbsh reports errors as values — no exceptions. Shell control
  flow (`exit`, `return`, `break`, `continue`) is a `FlowSignal` carried on
  the `Executor`; check `flowPending()` after running child nodes and
  consume the signal in the frame that owns the construct. Expansion
  errors are a pending message on the `Expander` (`failed()` /
  `takeError()`). Numeric parsing goes through `numparse.h` (never bare
  `std::stoi`-family calls), filesystem calls use the `std::error_code`
  overloads, and `std::regex` only ever via `regexutil.h` — whose two
  adapters are the single designated exception boundary in the codebase.
  Don't add `try`/`catch`/`throw` anywhere else.

When in doubt, find a recent function in the same file that does something
similar and copy its shape.

### Automated style check

`tools/check_style.py` enforces the mechanically-checkable rules: tab
indentation, trailing whitespace, LF line endings, line length (≤120 hard,
warning at >100), `#pragma once` in headers, no anonymous namespaces,
function bodies ≤60 lines.

```sh
python tools/check_style.py                  # check src/
python tools/check_style.py src/foo.cpp      # check a specific file
python tools/check_style.py --warnings-as-errors
```

**You must run this before opening a PR.** Errors must be clean; warnings
should be addressed unless they're pre-existing in code your PR didn't
touch. The function-length check is a heuristic — if it misfires on
something that genuinely isn't a function (rare), call it out in the PR
description rather than disabling the rule.

The checker only catches the *mechanical* rules. It cannot tell you whether
your naming is good, whether your helper extraction makes sense, or whether
your comments explain the right things. Those still need a human review.

---

## PR checklist

Before opening the PR:

- [ ] Builds clean as `Release|x64`. No new warnings.
- [ ] `python tools/check_style.py` reports no errors on touched files.
- [ ] `tests/run-all.sh` passes.
- [ ] `WBSH_GOLDEN=1 tests/run-all.sh` passes (or expected files updated
      with a one-line note in the PR description about what changed).
- [ ] New behavior has at least one test script.
- [ ] No anonymous namespaces, no `{ ... }` scope blocks, no commented-out
      code added.
- [ ] Touched headers still self-contain (try compiling a TU that includes
      only the changed header).
- [ ] Public functions / types have Doxygen comments.

PR description should call out:

- What user-visible behavior changes, if any.
- Whether goldens were updated (and why).
- Anything platform-specific (we're Windows-only today, but a PR that
  *could* break on a future Linux port should say so).

---

## Reporting bugs

Open an issue with:

- wbsh version — the banner that prints from `wbsh --help`.
- Windows build (`winver`).
- Minimal reproducer: the smallest shell snippet or script that triggers
  the bug. Inline it in the issue rather than attaching a file unless it's
  truly large.
- Expected vs actual output. If the bug is a parse/expansion error, the
  output of `wbsh -t <file>` (token dump) and `wbsh <file>` (AST dump,
  default no-`-r` mode) is gold.

For crashes, a stack trace from a Debug build (`x64\Debug\wbsh.exe`) is
worth several rounds of guessing.

---

## Areas that especially want help

The roadmap in the README lists the major bets. Smaller things that would
land cleanly today:

- Continue the `coreutils.cpp` file-split (see above) — pure mechanical
  refactor, well-bounded, useful exercise for getting familiar with the
  codebase.
- Tab completion: the plumbing is there (`complete`, `compgen`, completion
  specs in `Executor`) but most built-in completions are stubs.
- Reverse-incremental search (`Ctrl-R`) in `lineedit.cpp`.
- A real CI workflow — even just "build Release and run `WBSH_GOLDEN=1
  run-all.sh`" on every push would be a meaningful upgrade.
- Filling in `tests/` coverage for areas that currently rely on smoke
  tests alone (job control, traps, `read -t`, here-strings in pipelines).

If you want to take on one of the roadmap items (ConPTY, process
substitution, package manager), open an issue first so we can align on
scope before you sink time into it.
