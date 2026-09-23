# wbshterm

A terminal for wbsh. This directory currently holds **M0**, the ConPTY
spike: enough plumbing to prove that wbsh can be hosted on a pseudoconsole
without changing a line of the shell.

There is no window yet. `spike_main.cpp` passes this process's own console
straight through, so what M0 demonstrates is the pipe layer that M1's
renderer will sit on top of.

## Build and run

`wbshterm.vcxproj` is part of `wbsh.sln`, so Visual Studio and msbuild
build it like any other project. It depends on `wbsh`, which puts
`wbsh.exe` next to `wbshterm-spike.exe` in `x64\$(Configuration)\` — where
the spike looks for it first.

```powershell
msbuild ..\wbsh.sln /t:wbshterm /p:Configuration=Release /p:Platform=x64
```

`build.ps1` builds the same sources without Visual Studio, to
`wbshterm\build\`. Both paths compile at `/W4 /WX` and run the style
checker; keep them flag-compatible.

```powershell
.\build.ps1                 # Release; -Configuration Debug for /Od /Zi
.\tests\smoke.ps1           # three scripted checks against a real session
.\tests\smoke.ps1 -Spike ..\x64\Release\wbshterm-spike.exe
```

Interactive passthrough, from a real console (Windows Terminal or conhost):

```powershell
.\build\wbshterm-spike.exe
```

Scripted, for tests and measurements — `\r` is Enter, and the feed should
end with `exit` so the session closes on its own:

```powershell
.\build\wbshterm-spike.exe --feed "ls -la\rexit\r"
.\build\wbshterm-spike.exe --feed "cat big.txt\rexit\r" --quiet   # throughput only
```

## What M0 established

**wbsh needs no changes.** Hosted on a pseudoconsole it boots, prints its
prompt, runs commands, and exits with the right status. Its line editor
(`ReadConsoleInputW`), its width queries (`GetConsoleScreenBufferInfo`) and
its raw-mode switching all work, because ConPTY gives the child a real
console rather than a bare pipe. `vim` from Git Bash renders and takes
input through the same session.

**ConPTY throughput is ~1.5 MB/s.** Measured by `cat`-ing ~970 KB of source
through a session (989 KB in 634 ms, including wbsh's own read). That is
the number to design the renderer against: a large dump is I/O-bound in the
pty, not in our painting, so M1 should coalesce reads and paint at most
once per frame rather than per read.

**One Win32 quirk decides whether any of this works.** The child must be
spawned with `STARTF_USESTDHANDLES` and all three std handles set to
`nullptr`:

```cpp
startup.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;
startup.StartupInfo.hStdInput  = nullptr;
startup.StartupInfo.hStdOutput = nullptr;
startup.StartupInfo.hStdError  = nullptr;
```

Without it — and the SDK's own EchoCon sample omits it — the child inherits
*this* process's standard handles on Windows 10 22H2. The failure is
peculiar enough to be worth recognising: the child is genuinely attached to
the pseudoconsole, so `mode con` reports the pty's size, while everything
written to stdout bypasses the pty and lands in the parent's own output.
Only explicit `> CON` writes come back through the pipe.

## Files

| File | Contents |
| --- | --- |
| `src/pty.h` / `src/pty.cpp` | `PtySession`: pseudoconsole, pipes, child lifetime. Seeds M1's `pty.cpp`. |
| `src/spike_main.cpp` | Console passthrough, `--feed` mode, throughput reporting. Thrown away at M1. |
| `build.ps1` | vswhere + `cl /W4 /WX`; no project file yet. |
| `tests/smoke.ps1` | Round trip, exit status, and a regression check for the std-handle quirk. |

## Teardown order

`read()` returns 0 only once the pseudoconsole is closed, and `close()`
releases the handle readers are blocked on. So the sequence is
`endSession()`, then join every reader, then `close()`. Interactive mode
also cancels the pending console read, since that thread is parked in
`ReadFile` on this process's input handle.
