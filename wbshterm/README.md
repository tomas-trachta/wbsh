# wbshterm

A terminal for wbsh: a Win32 window that hosts an unmodified `wbsh.exe` on a
pseudoconsole, parses its VT output into a cell grid, and paints that grid
with Direct2D and DirectWrite.

**M4 is done** — it is yours to make look right: nine colour schemes, any
font, cursor style, padding, line height and opacity, all in a config file
that reloads a second after you save it. Wide characters, emoji and CJK
render at the right width in the right fonts. Rewrapping text on resize is
the one M4 item left (see Remaining).

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
| `--config <path>` | Uses this settings file instead of the one in %APPDATA%. |

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
| `config.h/.cpp` | The settings file: parsing, defaults, and the file written on first run. |
| `theme.h/.cpp` | The built-in colour schemes. |
| `menu.h/.cpp` | The right-click menu: what it offers, and what a click meant. |
| `charwidth.h/.cpp` | How many cells a character takes: zero, one or two. |
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

## The startup panel

Opening a session prints a screenfetch-style panel: the wbsh logo, who and
where you are, the OS build, uptime, CPU, memory, display, and which theme
and font this terminal is using — then the sixteen palette colours, so a
theme shows itself off as the session opens.

Turn it off in the config:

```ini
[startup]
fetch = false
```

The panel is `wbshterm --fetch`, run by the shell rather than drawn by the
terminal: ConPTY owns the screen, so anything the terminal painted itself
would be wiped by the shell's first repaint. wbshterm sets
`WBSH_INIT_COMMAND`, which wbsh runs once its session is up, just after
`.wbshrc`. Run `wbshterm --fetch` yourself any time.

## Making it yours

**Right-click the terminal.** The menu carries the settings people actually
reach for — theme, font size, cursor style and blinking, opacity, padding —
plus Copy and Paste, and it ticks whatever is currently set. The last two
entries open the configuration file and the themes folder, creating the
folder and its example if they are not there yet. A choice
applies at once *and* is written back to the config file, so it is still
there tomorrow; the rest of the file, comments included, is left exactly as
you wrote it. The last entry opens the file itself in your editor.

`Ctrl+=` and `Ctrl+-` change the font size from the keyboard, `Ctrl+0`
puts it back.

The first run writes a commented config to
`%APPDATA%\wbshterm\wbshterm.conf` and watches it: save the file and the
terminal picks the change up within a second, no restart. `--config <path>`
points at a different one.

```ini
[font]
family = Cascadia Mono
size = 11
line_height = 1.15
fallback = Segoe UI Emoji, Microsoft YaHei UI, Segoe UI Symbol

[window]
padding = 10
opacity = 0.95

[cursor]
style = bar        # block, bar or underline
blink = true

[theme]
name = tokyo-night
foreground = #C0CAF5    # anything set here overrides the named theme
```

Nine schemes ship: `catppuccin-mocha` (the default), `tokyo-night`,
`dracula`, `nord`, `gruvbox-dark`, `one-dark`, `solarized-dark`,
`solarized-light`, `vscode-dark`.

### Your own themes

The themes folder holds every shipped theme as a file — `nord.conf`,
`dracula.conf` and the rest — so you can read them, edit them, or copy one
as a starting point. Right-click and choose **Open themes folder…** to get
there.

Editing a theme's file changes that theme, and the terminal picks the change
up within a second like any other setting. Delete a file and the built-in
comes back. Add `midnight.conf` and you have a theme called `midnight`,
listed in the menu beside the others and selectable with `name = midnight`.

A theme file is the same text as the `[theme]` section, so either can be
pasted into the other; a section header in it is ignored. Every key is
optional, and colours written in `wbshterm.conf` still win over the theme
they name.

A theme is nothing but twenty colours — `background`, `foreground`,
`cursor`, `selection`, and `ansi0` through `ansi15` — and every one of them
is a key you can set yourself. `name` picks the starting point; anything you
write alongside it wins, wherever in the section you write it. Leave `name`
out entirely and the twenty keys are yours alone:

```ini
[theme]
background = #11121B
foreground = #C8D0E0
cursor     = #F2CDCD
selection  = #3A3F58
ansi0 = #11121B
ansi1 = #F07178
# ... through ansi15
```

Colours are resolved when a cell is painted, not when the text arrived, so
switching themes recolours what is already on screen rather than only what
comes next.

Previewing a theme needs no window:

```powershell
.\build\wbshterm.exe --snapshot preview.png --config theme.conf --feed "ls --color\r"
```

## Text that is not ASCII

`charwidth.cpp` decides how many cells a character claims — zero for
combining marks, two for CJK, fullwidth forms and emoji — and the grid
keeps a trailing cell for the second half so the shell and the terminal
agree about where the cursor is. Glyphs the main font lacks come from the
`fallback` families, and colour emoji are drawn in colour where the target
supports it.

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
| Customise | Right-click, or Shift+F10 |
| Font size | Ctrl+=, Ctrl+-, Ctrl+0 |
| Return to the bottom | Type anything |

Two behaviours are deliberate. New output does **not** yank the view back
to the bottom while you are reading history — the scroll offset grows by
however many lines arrived, so the text under your eyes stays put — and the
cursor is not painted while scrolled back, because it belongs to the live
grid rather than to what is on screen.

## Remaining

**Text does not rewrap when the window is resized.** Content is carried
over — the grid keeps what fits and pushes the rest into scrollback — but a
line that wrapped at the old width stays broken where it was. Rewrapping
needs the grid to record where a line continues, which is the next piece of
work here.

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

**Astral characters cannot be typed into wbsh.** Paste or type an emoji
and the shell echoes U+FFFD twice, once per UTF-16 half — the bytes are
already replacement characters when they come back out of the pseudoconsole,
so nothing downstream can recover them. The same emoji printed *by* the
shell (`cat` a file containing one) arrives intact and renders correctly, so
this is wbsh's line editor reading key events one UTF-16 unit at a time, not
a terminal bug.

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

**A shell that exits does not close the pipe.** ConPTY holds the output
pipe open after the child is gone, so a reader waiting on bytes never learns
the shell quit — the window stayed open on Ctrl-D while `exit` worked, since
that path happened to produce output first. The session now waits on the
child process as well as on its output, and closes the window either way.

## Teardown order

`read()` returns 0 only once the pseudoconsole is closed, and `close()`
releases the handle readers are blocked on. So the sequence is
`endSession()`, then join every reader, then `close()` — which is what
`Session::stop()` does.
