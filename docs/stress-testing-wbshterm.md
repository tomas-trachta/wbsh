# Stress-test plan for wbshterm

This plan tells how we stress-test wbshterm, the terminal that hosts wbsh.
It has the same five areas as the wbsh plan
([`stress-testing.md`](stress-testing.md)): memory, speed, common bugs,
security and user experience. The automatic part is the framework in
[`wbshterm/stress/`](../wbshterm/stress/README.md). It shares the process
probe, findings and reports with the wbsh framework (`tools/stress/`).

## 1. Scope

In scope:

- The VT parser and the cell grid (escape sequences, Unicode, scrollback).
- Sessions: the pseudoconsole (bundled `conpty.dll` / `OpenConsole.exe`), input, output, the hosted wbsh.
- The window: rendering, panes, selection, clipboard, links, the picker, the status bar, configuration and Lua scripts.
- The headless modes (`--replay`, `--snapshot`, `--selftest`, `--fetch`), because tests and CI use them.

Out of scope: wbsh itself (its own plan), other shells hosted with `--shell`.

## 2. Release criteria

| # | Area | Criterion |
|---|---|---|
| T1 | Bugs | The built-in `--selftest` has no `FAIL` line. `tests/smoke.ps1` passes. |
| T2 | Bugs | Every golden grid (`suites/bugs/vt/`) matches, or its difference is a written decision. |
| T3 | Bugs | 0 crashes and 0 hangs in 50 000 fuzz streams (several seeds) and in the corpus at all grid sizes. |
| T4 | Bugs | Every session case passes: echo, colours, vi, less, clear, wrap, Unicode, Tab, history, `read -t`, line drawing. |
| T5 | Memory | No `leak` finding. A 2-hour window soak (`-Gui`, `-Mode full`) keeps memory, handles, GDI and USER objects flat. |
| T6 | Speed | No regression > 25 % against the baseline. Replay ≥ 20 MB/s for plain text. A session reaches a finished first command in < 300 ms. |
| T7 | Security | No `critical` or `high` finding: no reply carries program-chosen bytes, pasted text cannot end bracketed paste, links open only allowed schemes, scrollback files do not stay on disk. |
| T8 | Security | `wbshterm.exe`, `conpty.dll` and `OpenConsole.exe` have ASLR and DEP; the vendored binaries have a recorded SHA-256; everything shipped is code-signed. |
| T9 | UX | Bad command-line input gives an error exit, not a crash, a hang or a modal dialog in a headless mode. Broken settings files fall back to defaults. |
| T10 | UX | The manual checklist (section 5) is complete on two machines (one high-DPI, one 100 %). |

Severity has the same meaning as in the wbsh plan (section 3 there).

## 3. Method per area

### 3.1 Memory

- **Replay scaling** (`suites/memory/*.vt.ps1`): each stream runs at N and
  10 × N. Scrollback (10 000 lines), links (65 535) and clusters (65 536)
  are capped, so memory must not follow the input. Replay keeps its input
  file in memory, so the allowance is `ratio` × the extra input.
  A case must fill its caps at both scales (or avoid the scrollback), or it
  measures normal growth instead of a leak.
- **Session scaling** (`session_output`): ten times more output through
  the real pseudoconsole; the disk scrollback must keep wbshterm flat.
- **Window soak** (`-Gui`, `soak.window.ps1`): the real window runs a script
  that prints colour, titles, clears and the alternate screen. The probe
  samples private bytes, handles, GDI and USER objects every 20 ms; the
  last quarter must not be above the first.
- **Manual:** an ASan build (`-p:EnableASAN=true`, as for wbsh) through
  `stress.ps1 -Exe <asan wbshterm.exe> -Suite bugs`; a working-day soak with
  real use (many panes, resizes, themes changed live).

### 3.2 Speed

- **Replay throughput** (`suites/perf/*.vt.ps1`): plain text, 256 colours,
  true colour, cursor addressing, Unicode, scroll regions, long wrapped
  lines, alternate-screen frames, insert/delete. The report gives MB/s.
- **Sessions**: time to a finished first command, and 200 000 lines through
  the pseudoconsole.
- **Manual:** frame time while scrolling and resizing a full window
  (Windows Performance Recorder, GPU view); input latency (key press to
  paint) with a high-speed camera or `typometer`; cold start after a reboot.

### 3.3 Common bugs

- **Self-test** (`--selftest`): the project's own 490 checks.
- **Golden grids** (`suites/bugs/vt/`): small hand-checked cases of the VT
  rules: cursor moves, erase, autowrap and the pending-wrap state, margins,
  insert/delete, tabs, wide and combining characters, alternate screen,
  origin mode, DEC line drawing, invalid UTF-8, huge parameters.
- **Robust streams** (`suites/bugs/robust/`): counts near 2³¹, 20 MB OSC,
  DCS, APC strings, a million parameters, a million combining marks, resize
  requests, mode storms, 20 MB of random bytes — each with a time and memory budget.
- **Corpus and fuzzing**: hostile byte files at three grid sizes (1×1,
  80×24, 500×3), and random or mutated streams at random sizes.
- **Sessions** (`suites/bugs/sessions/`): real programs in wbsh inside the terminal.
- **Manual:** vttest (cursor movements, screen features, character sets);
  real programs: vim, htop-like `top`, `tmux`, `mc`, `git log --graph`,
  `less` with long lines, `fzf`, programs over `ssh`.

### 3.4 Security

The terminal shows output that other people control: files, logs, web
pages, remote hosts. Threats:

| Threat | Test |
|---|---|
| A reply to a query carries program-chosen text, which the shell reads as typed input (title report, DECRQSS, OSC 52 read). | `security/replies.session.ps1` |
| Pasted text holds `ESC[201~` and ends bracketed paste early; the rest runs as commands. | `static_scan.ps1` (paste encoder); manual paste test below |
| A hyperlink opens a dangerous scheme (`file:`, `ms-msdt:`, a UNC path) on Ctrl+click. | `static_scan.ps1` (allowlist); manual |
| Output sets the clipboard (OSC 52) without the user knowing. | review item |
| Scrollback with secrets stays in `%TEMP%`. | `security/scrollback_file.session.ps1` |
| Huge or malformed sequences use all memory or CPU (denial of service). | robust streams, fuzzing |
| A swapped `conpty.dll` / `OpenConsole.exe`. | static scan (hash record), signing |
| A settings or Lua file from an untrusted place runs code. | review item (Lua has `os.execute`) |

### 3.5 User experience

- **Automatic:** `ux/cli.ps1` (arguments, errors, no dialogs in headless
  modes), `ux/config_fuzz.ps1` (broken settings files).
- **Manual:** section 5.

## 4. When to run what

| When | Command | Time |
|---|---|---|
| Before each wbshterm PR | `wbshterm\stress\stress.ps1 -Suite bugs,security,ux` and `tests\smoke.ps1` | ~2 minutes |
| A PR that touches the parser, grid or renderer | add `-Suite perf,memory` | ~3 minutes |
| Every night (when CI exists) | `-Mode full -Seed <date>` | ~20 minutes |
| Release candidate | `-Mode full -Gui` on two machines, ASan run, manual checklist | 1 day |

## 5. Manual checklist

Do this in the real window, on a 100 % and a high-DPI monitor.

- [ ] **Ctrl+C** stops `sleep 30`, `ping -t localhost` and `seq 1 100000000 | cat`; the prompt comes back. (Headless sessions do not deliver Ctrl+C; see section 6.)
- [ ] **Paste injection:** copy the text `echo safe` + ESC `[201~` + `echo INJECTED` + Enter (make it with `printf 'echo safe\033[201~echo INJECTED\n' | clip`), paste it at the prompt: nothing may run before you press Enter, and `INJECTED` must not run on its own.
- [ ] **Links:** `printf '\e]8;;file:///C:/Windows/System32/calc.exe\e\\calc\e]8;;\e\\\n'`, then Ctrl+click: nothing opens. An `https://` link opens the browser.
- [ ] **OSC 52:** `printf '\e]52;c;%s\a' "$(printf hijacked | base64)"`: decide if the clipboard may change without a prompt.
- [ ] Resize quickly during `seq 1 1000000`, in `vi`, `less` and `top`; drag between monitors with different DPI.
- [ ] Panes: split ten times, close in random order, resize the dividers, run output in all of them.
- [ ] Selection and copy: across wrapped lines, wide characters, the alternate screen; copy a command's output on its own.
- [ ] IME (Japanese, Chinese) and dead keys / AltGr on a Czech keyboard.
- [ ] Narrator or NVDA: the prompt, output and selection are read.
- [ ] Themes and fonts changed while running; a font without the needed glyphs (fallback).
- [ ] The picker (`fzf`), the status bar, the start panel, "Open wbshterm here" in Explorer.
- [ ] Sleep and resume, remote desktop reconnect, 8 hours left open.

## 6. First run (2026-11-11, wbshterm 1.0.32, quick profile)

All items were reproduced by hand.

**Blocking:**

| # | Area | Finding | Case |
|---|---|---|---|
| W1 | Security | `encodePaste` (`keymap.cpp`) does not remove ESC from pasted text, so `ESC[201~` inside a paste ends bracketed paste and the rest arrives as typed input. | `security/static_scan.ps1` |
| W2 | UX | `--size 100000x100000` and `--scroll many` crash wbshterm (an uncaught exception, fail-fast exit). `--delay -5` waits forever (the negative value wraps). | `ux/cli.ps1` |
| W3 | UX | In headless modes every error opens a modal `MessageBox`, so a script or CI job waits until someone clicks. | `ux/cli.ps1` |
| W4 | Bugs | 3 built-in self-test checks fail on `main`: "case folding covers more than ASCII", "the terminal's choice reaches the shell", "the shell acts on what was chosen". | `selftest` |
| W5 | wbsh | `read -t 1` never times out in an interactive wbsh on the pseudoconsole. | `sessions/read_timeout` |
| W6 | wbsh | Input typed in the second after leaving `less` with `q` is lost. | `sessions/less_pager` |

**Medium / low:**

- Leaving the alternate screen (mode 1049) does not restore the cursor (`vt/alt_screen`).
- Writing over the right half of a wide character leaves its left half (`vt/wide_overwrite`).
- DEC special graphics (`ESC ( 0`) are not supported, also not through the pseudoconsole: box-drawing programs show `lqqk` (`vt/line_drawing`, `sessions/line_drawing`).
- DECALN (`ESC # 8`) does nothing (`vt/decaln`); low.
- `--record` appends to an existing file instead of replacing it.
- Ctrl+C does not reach a command in headless sessions, neither as byte 0x03 nor as a win32-input-mode key event. Check the real window (section 5) before deciding if this is a product bug.

**Clean:** 0 crashes in 400 fuzz streams and the corpus at three grid sizes;
all robust streams inside their budgets (a 20 MB title costs 90 MB peak, a
million combining marks 7 MB); memory flat at 10× input for scrollback,
links, clusters, titles, shell marks, alternate screen and a live session;
no reply carries program-chosen bytes; the scrollback file is gone after
exit; links have a scheme allowlist; broken settings files fall back to
defaults; ASLR and DEP on all three binaries (CFG only on the two Microsoft ones; build wbshterm.exe with `/guard:cf`).

**Speed** (quick profile): replay 12–122 MB/s (scroll regions slowest,
true colour fastest); a session reaches its first finished command in
~180 ms; 200 000 lines through the pseudoconsole in 4.5–5.8 s.

### 6.1 Status after the first fix round (branch `fix/stress-findings`)

Fixed: W1 (paste drops control characters; two self-test checks), W2 and
W3 (checked numbers with limits, errors to stderr instead of a dialog),
W4 (final sigma folds to sigma; the stress driver also starts the
self-test in the repository root, which the picker checks need), W5
(`read -t` in wbsh), DEC special graphics, DECALN, wide-character halves,
the pending wrap after the alternate screen, and `--record` starting a
fresh file. The self-test now has 496 checks and all pass; all 25 golden
grids pass.

Still open: W6 (input after `less`), Ctrl+C in snapshot sessions, CFG
(`/guard:cf`) for wbshterm.exe, a recorded SHA-256 for the vendored
ConPTY binaries. The self-test check "quitting a pager leaves a working
prompt" passes even when the command does not run: it finds its marker
in the echo of the typed line.

## 7. Next steps for the framework

1. A `--feed` notation for key events that the snapshot session delivers like the window does (Ctrl+C, resize), so the Ctrl+C case can become blocking.
2. A `--chunk <n>` option for `--replay` (feed the parser n bytes at a time) and a check that the grid does not depend on the chunk size: the pseudoconsole splits output at any byte.
3. Screenshot goldens for the renderer (`--snapshot`), compared with a tolerance.
4. Resize during replay (a `\e[8;rows;cols t` honoured by `--replay`, or a `--resize-at` option) to test rewrap when it lands.
5. UI Automation tests of the window (selection, panes, menus) with the existing `uia` provider.
