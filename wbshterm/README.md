# wbshterm

A terminal for wbsh: a Win32 window that hosts an unmodified `wbsh.exe` on a
pseudoconsole, parses its VT output into a cell grid, and paints that grid
with Direct2D and DirectWrite.

**M1 is done** — the window renders. Input is a deliberate stub (typing,
Enter, backspace, arrows, Home/End, Delete, PageUp/Down); the full encoder,
scrollback and selection are M2 and M3.

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

`--feed "ls --color\r"` types into any session (`\r` is Enter), `--size
100x30` sets the grid, and `--shell` / `--args` pick a different program to
host.

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
| `session.h/.cpp` | Pty + parser + grid + reader thread; the grid is touched by one thread only. |
| `font.h/.cpp` | DirectWrite faces and the measured cell box. |
| `render.h/.cpp` | Direct2D painting of a grid onto any render target. |
| `window.h/.cpp` | The Win32 window, message handlers, and key encoding. |
| `snapshot.h/.cpp` | Off-screen WIC target and PNG encode. |
| `replay.h/.cpp` | Recording in, grid out, no shell involved. |
| `selftest.h/.cpp` | The headless checks. |
| `main.cpp` | Argument parsing and mode dispatch. |

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

## Teardown order

`read()` returns 0 only once the pseudoconsole is closed, and `close()`
releases the handle readers are blocked on. So the sequence is
`endSession()`, then join every reader, then `close()` — which is what
`Session::stop()` does.
