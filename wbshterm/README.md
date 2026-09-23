# wbshterm

A terminal for wbsh: a Win32 window that hosts an unmodified `wbsh.exe` on a
pseudoconsole, parses its VT output into a cell grid, and paints that grid
with Direct2D and DirectWrite.

**M3 is done** — the window renders, takes real input, and remembers:
10,000 lines of scrollback with wheel and keyboard scrolling, mouse
selection with copy, and an alt screen. Reflow, wide glyphs, fonts and
DPI polish are M4.

## Build and run

`wbshterm.vcxproj` is part of `wbsh.sln`, so Visual Studio and msbuild build
it like any other project. It depends on `wbsh`, which puts `wbsh.exe` next
to `wbshterm.exe` in `x64\$(Configuration)\` — where the terminal looks for
it first.

```powershell
msbuild ..\wbsh.sln /t:wbshterm /p:Configuration=Release /p:Platform=x64
```

`build.ps1` builds the same sources without Visual Studio, to
`wbshterm\build\`. Both paths compile at `/W4 /WX` and run the style
checker; keep them flag-compatible.

```powershell
.\build.ps1
.\build\wbshterm.exe                 # the terminal
.\tests\smoke.ps1                    # self-test, snapshot, replay determinism
```

## Modes

The window is the point, but the headless modes are how this thing is
tested — a terminal that can only be checked by looking at it cannot be
checked in CI.

| Mode | What it does |
| --- | --- |
| *(no arguments)* | Opens the window on a wbsh session. |
| `--selftest <report.txt>` | Parser and grid checks plus one live pty session; exit code is the verdict. |
| `--snapshot <out.png>` | Runs a session headlessly and paints the finished grid to a PNG. |
| `--record <out.raw>` | With `--snapshot`, also writes every byte the pty produced. |
| `--replay <in.raw>` | Parses a recording with no shell; `--dump <txt>` writes the grid as text, `--snapshot` paints it. |
| `--scroll <n>` | With `--snapshot`, scrolls back n lines before painting. |
| `--select <r:c-r:c>` | With `--snapshot`, highlights a selection in absolute rows. |

`--feed "ls --color\r"` types into any session (`\r` is Enter, `\e` is
Escape, and `\x1b[A` spells any byte), `--size 100x30` sets the grid, and
`--shell` / `--args` pick a different program to host.

Record-then-replay is the test strategy: a recording is deterministic, so a
grid dump taken from one is a golden file, and a rendering bug can be told
apart from a parsing bug by replaying the same bytes.

```powershell
.\build\wbshterm.exe --snapshot out.png --feed "ls --color -la\r" --size 100x24
.\build\wbshterm.exe --replay session.raw --dump grid.txt --size 100x24
```

## Modules

| File | Responsibility |
| --- | --- |
| `pty.h/.cpp` | `PtySession`: pseudoconsole, pipes, child lifetime. |
| `vtparse.h/.cpp` | Bytes to VT actions (DEC state machine) and UTF-8 decoding. |
| `screen.h/.cpp` | The cell grid: printing, cursor motion, erase, scroll, SGR, query replies. |
| `keymap.h/.cpp` | Key presses to bytes: modifiers, cursor-key modes, paste bracketing. |
| `view.h/.cpp` | What the window looks at: scroll position and selection, in absolute rows. |
| `session.h/.cpp` | Pty + parser + grid + reader thread; the grid is touched by one thread only. |
| `font.h/.cpp` | DirectWrite faces and the measured cell box. |
| `render.h/.cpp` | Direct2D painting of a grid onto any render target. |
| `window.h/.cpp` | The Win32 window, message handlers, and key encoding. |
| `snapshot.h/.cpp` | Off-screen WIC target and PNG encode. |
| `replay.h/.cpp` | Recording in, grid out, no shell involved. |
| `selftest.h/.cpp` | The headless checks. |
| `main.cpp` | Argument parsing and mode dispatch. |

## Input

`keymap.cpp` holds the encoding and nothing else — no window, no state — so
every combination is unit-tested rather than tried by hand.

| Key | Sent |
| --- | --- |
| Arrows, Home, End | `CSI A`…`CSI F`, or `SS3` in application cursor mode (DECCKM) |
| With modifiers | `CSI 1;<mod><final>`, where mod is 1 + shift + 2·alt + 4·ctrl |
| Insert, Delete, PageUp/Down | `CSI 2~`, `CSI 3~`, `CSI 5~`, `CSI 6~`, modified as `CSI 3;2~` |
| F1–F4 / F5–F12 | `SS3 P`…`SS3 S` / `CSI 15~`…`CSI 24~` |
| Backspace | DEL (0x7f); Ctrl+Backspace sends BS (0x08) |
| Ctrl+Space | NUL |
| Shift+Tab | `CSI Z` |
| Alt+*key* | the key's bytes, ESC-prefixed |
| Ctrl+V, Shift+Insert | paste, wrapped in `CSI 200~`/`CSI 201~` when the shell asked for bracketed paste |

Plain typing is deliberately *not* encoded here: a press that carries no
meaning of its own falls through to the character message Windows already
produced, which is how layouts, dead keys and AltGr keep working without
this file knowing they exist. The one wrinkle is that Windows also produces
a character for a few keys that are handled here — Ctrl+Space, Shift+Tab,
Ctrl+V — so those, and only those, swallow the character that follows.

## Scrollback and selection

Lines that scroll off the top are kept — 10,000 of them — and the view
addresses scrollback and the live grid as one coordinate space, so a
selection stays on the text it was made on no matter what arrives
afterwards.

| Action | Binding |
| --- | --- |
| Scroll | Wheel, Shift+PageUp/PageDown, Ctrl+Shift+Up/Down |
| Select | Drag; double click for a word, triple for the line |
| Copy | Ctrl+Shift+C, Ctrl+Insert |
| Return to the bottom | Type anything |

Two behaviours are deliberate. New output does **not** yank the view back
to the bottom while you are reading history — the scroll offset grows by
however many lines arrived, so the text under your eyes stays put — and the
cursor is not painted while scrolled back, because it belongs to the live
grid rather than to what is on screen.

## Things learned the hard way

**The child needs `STARTF_USESTDHANDLES` with all three std handles null**
(M0). Without it — and the SDK's own EchoCon sample omits it — the child
inherits *this* process's standard handles on Windows 10 22H2. The failure
is peculiar: the child is genuinely attached to the pseudoconsole, so `mode
con` reports the pty's size, while everything it writes to stdout bypasses
the pty and lands in the host's output. Only explicit `> CON` writes come
back. `--selftest` guards it.

**Queries must be answered.** Full-screen applications send DA1 (`ESC[c`),
DA2 (`ESC[>c`) and DSR/CPR (`ESC[6n`) and change behaviour based on the
reply — or on its absence. `Screen` answers them through a `VtResponder`,
which `Session` implements by writing back into the pty.

**ConPTY output is conhost's rendering, not the application's.** What
arrives on the pipe is regenerated from conhost's own buffer, so an oddity
in the stream is not necessarily a bug in this terminal: vim under ConPTY
emits `ESC[67C` followed by a literal `m`, and every conformant terminal
(this one, Windows Terminal) paints that `m`.

**ConPTY throughput is ~1.5 MB/s** (M0: 989 KB in 634 ms). The pty, not the
renderer, is the ceiling on a large dump. The reader thread only buffers
bytes and posts one wake-up at a time; parsing and painting happen once per
drain on the UI thread.

**Backspace is DEL, not BS.** ConPTY follows the xterm convention: 0x7f
is Backspace and 0x08 is Ctrl+Backspace. Windows hands the window 0x08
for a plain press, so passing the character message straight through
arrives at the shell as Ctrl+Backspace and erases a word, or nothing.
Backspace and Delete both have live checks in `--selftest` now.

**ConPTY never forwards the alt screen.** `less` and `vim` under a
pseudoconsole never produce `CSI ?1049h`: conhost renders them into its own
main buffer and streams the result, so the switch never crosses the pipe.
The alt screen is implemented and unit-tested here for streams that do use
it, but under ConPTY the behaviour that matters is what `--selftest`
checks — quitting a pager leaves a working prompt and the scrollback that
was there before.

**A resize is not real until the shell sees it.** `ResizePseudoConsole` is
only half of it: wbsh republishes `COLUMNS` when it next draws a prompt, so
a test that resizes and immediately asks reads the old width. The check in
`--selftest` gives it a prompt first.

## Teardown order

`read()` returns 0 only once the pseudoconsole is closed, and `close()`
releases the handle readers are blocked on. So the sequence is
`endSession()`, then join every reader, then `close()` — which is what
`Session::stop()` does.
