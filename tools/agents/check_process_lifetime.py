#!/usr/bin/env python3
"""
Process-lifetime and console checks for wbsh as an agent host starts it.

    kill        The host kills wbsh (TerminateProcess) while it waits for a
                native command that has a native child of its own: both end,
                and the pipes close. `&`, `& disown`, `nohup ... &` and
                `setsid -f` children live on.
    exit        A foreground command leaves a child running and wbsh exits
                normally: the child lives on.
    no-console  wbsh started without a console (DETACHED_PROCESS) or with a
                hidden one (CREATE_NO_WINDOW), stdio on pipes, runs console
                programs: none of them gets a console window.

The no-console check runs wbsh on a desktop of its own that is never shown,
so even a failing run cannot flash a window on the user's screen; when that
desktop cannot be made the check is skipped, never run on the visible one.

Usage:
    python -I tools/agents/check_process_lifetime.py [--wbsh PATH] [--verbose]

check_agent_shapes.py runs these checks too. Exit status: 0 when every check
passes, 1 on any failure, 2 on bad usage.
"""

from __future__ import annotations

import argparse
import ctypes
import msvcrt
import os
import shutil
import subprocess
import sys
import tempfile
import time
from ctypes import wintypes
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional


REPO = Path(__file__).resolve().parents[2]
DEFAULT_WBSH = REPO / "x64" / "Release" / "wbsh.exe"

CREATE_NO_WINDOW = 0x08000000
DETACHED_PROCESS = 0x00000008
CREATE_UNICODE_ENVIRONMENT = 0x00000400
STARTF_USESTDHANDLES = 0x00000100
HANDLE_FLAG_INHERIT = 0x00000001
GENERIC_ALL = 0x10000000
PROCESS_TERMINATE = 0x0001
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
STILL_ACTIVE = 259
INFINITE = 0xFFFFFFFF

START_TIMEOUT = 30.0
KILL_GRACE = 5.0
RUN_TIMEOUT = 60.0

SLEEPER = """\
import os, sys, time
with open(sys.argv[1], "w") as f:
    f.write(str(os.getpid()))
time.sleep(120)
"""

SPAWNER = """\
import os, subprocess, sys, time
sleeper, own, child, mode = sys.argv[1:5]
streams = None if mode == "wait" else subprocess.DEVNULL
subprocess.Popen([sys.executable, "-I", sleeper, child], stdin=subprocess.DEVNULL,
                 stdout=streams, stderr=streams)
with open(own, "w") as f:
    f.write(str(os.getpid()))
if mode == "wait":
    time.sleep(120)
"""

CONSOLE_PROBE = """\
import ctypes, sys
kernel32 = ctypes.windll.kernel32
window = kernel32.GetConsoleWindow()
print(f"{sys.argv[1]}: console={kernel32.GetConsoleCP() != 0} window={bool(window)}")
"""

SURVIVORS = ("bg", "disown", "nohup", "setsid")
CASUALTIES = ("fg", "fg-child")


@dataclass
class Outcome:
    name: str
    ok: Optional[bool]
    detail: str = ""


@dataclass
class Box:
    wbsh: Path
    root: Path

    def helper(self, name: str) -> str:
        return str(self.root / name)

    def pid_file(self, role: str) -> Path:
        return self.root / f"{role}.pid"

    def env(self) -> dict[str, str]:
        env = dict(os.environ)
        for name in ("BASH_ENV", "ENV", "SHELLOPTS", "BASHOPTS", "WBSH_INIT_COMMAND"):
            env.pop(name, None)
        env["PROBE_PY"] = sys.executable
        env["PROBE_SLEEPER"] = self.helper("sleeper.py")
        env["PROBE_SPAWNER"] = self.helper("spawner.py")
        env["PROBE_CONSOLE"] = self.helper("console_probe.py")
        env["PROBE_DIR"] = str(self.root).replace("\\", "/")
        return env


def make_box(wbsh: Path) -> Box:
    root = Path(tempfile.mkdtemp(prefix="wbsh-lifetime-"))
    for name, text in (("sleeper.py", SLEEPER), ("spawner.py", SPAWNER),
                       ("console_probe.py", CONSOLE_PROBE)):
        (root / name).write_text(text, encoding="utf-8", newline="\n")
    return Box(wbsh, root)


def kernel32() -> ctypes.WinDLL:
    library = ctypes.WinDLL("kernel32", use_last_error=True)
    library.OpenProcess.restype = wintypes.HANDLE
    library.CloseHandle.argtypes = [wintypes.HANDLE]
    library.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    library.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
    library.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    return library


KERNEL32 = kernel32()


def process_alive(pid: int) -> bool:
    handle = KERNEL32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return False
    code = wintypes.DWORD()
    KERNEL32.GetExitCodeProcess(handle, ctypes.byref(code))
    KERNEL32.CloseHandle(handle)
    return code.value == STILL_ACTIVE


def terminate(pid: int) -> None:
    handle = KERNEL32.OpenProcess(PROCESS_TERMINATE, False, pid)
    if handle:
        KERNEL32.TerminateProcess(handle, 1)
        KERNEL32.CloseHandle(handle)


def wait_until(predicate: Callable[[], bool], seconds: float) -> bool:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.05)
    return predicate()


def read_pid(path: Path) -> Optional[int]:
    try:
        text = path.read_text().strip()
    except OSError:
        return None
    return int(text) if text else None


def wait_for_pids(box: Box, roles: tuple[str, ...]) -> dict[str, Optional[int]]:
    wait_until(lambda: all(read_pid(box.pid_file(role)) for role in roles), START_TIMEOUT)
    return {role: read_pid(box.pid_file(role)) for role in roles}


def start_wbsh(box: Box, script: str) -> subprocess.Popen:
    return subprocess.Popen([str(box.wbsh), "-c", script], cwd=str(box.root), env=box.env(),
                            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, creationflags=CREATE_NO_WINDOW)


# The survivors start with the group's redirections so none of them holds
# the host's pipes; the foreground command and its child hold them, so the
# pipes close only once the kill has ended both.
KILL_SCRIPT = """\
{
    "$PROBE_PY" -I "$PROBE_SLEEPER" "$PROBE_DIR/bg.pid" &
    "$PROBE_PY" -I "$PROBE_SLEEPER" "$PROBE_DIR/disown.pid" & disown
    nohup "$PROBE_PY" -I "$PROBE_SLEEPER" "$PROBE_DIR/nohup.pid" &
    setsid -f "$PROBE_PY" -I "$PROBE_SLEEPER" "$PROBE_DIR/setsid.pid"
} </dev/null >/dev/null 2>&1
"$PROBE_PY" -I "$PROBE_SPAWNER" "$PROBE_SLEEPER" "$PROBE_DIR/fg.pid" "$PROBE_DIR/fg-child.pid" wait
echo not reached
"""


def judge_kill(pids: dict[str, Optional[int]], pipes_closed: bool) -> Outcome:
    missing = [role for role, pid in pids.items() if pid is None]
    if missing:
        return Outcome("kill", False, f"never started: {', '.join(missing)}")
    lingering = [role for role in CASUALTIES if process_alive(pids[role])]
    gone = [role for role in SURVIVORS if not process_alive(pids[role])]
    problems = []
    if lingering:
        problems.append(f"still running after the kill: {', '.join(lingering)}")
    if gone:
        problems.append(f"ended with the shell: {', '.join(gone)}")
    if not pipes_closed:
        problems.append("stdout/stderr pipes stayed open")
    detail = "; ".join(problems) or "fg command and its child ended; bg, disown, nohup, setsid live on"
    return Outcome("kill", not problems, detail)


def check_kill(box: Box) -> Outcome:
    process = start_wbsh(box, KILL_SCRIPT)
    pids = wait_for_pids(box, SURVIVORS + CASUALTIES)
    process.kill()
    wait_until(lambda: not any(pids[role] and process_alive(pids[role]) for role in CASUALTIES),
               KILL_GRACE)
    try:
        process.communicate(timeout=KILL_GRACE)
        pipes_closed = True
    except subprocess.TimeoutExpired:
        pipes_closed = False
    outcome = judge_kill(pids, pipes_closed)
    for pid in pids.values():
        if pid:
            terminate(pid)
    if not pipes_closed:
        process.communicate()
    return outcome


EXIT_SCRIPT = """\
"$PROBE_PY" -I "$PROBE_SPAWNER" "$PROBE_SLEEPER" "$PROBE_DIR/leaver.pid" "$PROBE_DIR/left.pid" exit
"""


def check_exit(box: Box) -> Outcome:
    process = start_wbsh(box, EXIT_SCRIPT)
    try:
        _, stderr = process.communicate(timeout=RUN_TIMEOUT)
    except subprocess.TimeoutExpired:
        process.kill()
        process.communicate()
        return Outcome("exit", False, "wbsh did not exit")
    left = wait_for_pids(box, ("left",))["left"]
    if process.returncode != 0 or left is None:
        return Outcome("exit", False, f"status {process.returncode}, stderr {stderr[:160]!r}")
    time.sleep(0.5)
    survived = process_alive(left)
    terminate(left)
    detail = "child left by a foreground command lives on" if survived else "child ended with the shell"
    return Outcome("exit", survived, detail)


class StartupInfo(ctypes.Structure):
    _fields_ = [
        ("cb", wintypes.DWORD), ("lpReserved", wintypes.LPWSTR), ("lpDesktop", wintypes.LPWSTR),
        ("lpTitle", wintypes.LPWSTR), ("dwX", wintypes.DWORD), ("dwY", wintypes.DWORD),
        ("dwXSize", wintypes.DWORD), ("dwYSize", wintypes.DWORD),
        ("dwXCountChars", wintypes.DWORD), ("dwYCountChars", wintypes.DWORD),
        ("dwFillAttribute", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
        ("wShowWindow", wintypes.WORD), ("cbReserved2", wintypes.WORD),
        ("lpReserved2", ctypes.c_void_p), ("hStdInput", wintypes.HANDLE),
        ("hStdOutput", wintypes.HANDLE), ("hStdError", wintypes.HANDLE),
    ]


class ProcessInformation(ctypes.Structure):
    _fields_ = [("hProcess", wintypes.HANDLE), ("hThread", wintypes.HANDLE),
                ("dwProcessId", wintypes.DWORD), ("dwThreadId", wintypes.DWORD)]


def user32() -> ctypes.WinDLL:
    library = ctypes.WinDLL("user32", use_last_error=True)
    library.CreateDesktopW.restype = wintypes.HANDLE
    library.CloseDesktop.argtypes = [wintypes.HANDLE]
    return library


USER32 = user32()


def hidden_desktop(name: str) -> Optional[int]:
    return USER32.CreateDesktopW(name, None, None, 0, GENERIC_ALL, None)


def environment_block(env: dict[str, str]) -> ctypes.Array:
    text = "".join(f"{name}={value}\0" for name, value in sorted(env.items())) + "\0"
    return ctypes.create_unicode_buffer(text, len(text))


def inheritable_pipe() -> tuple[int, int]:
    read_fd, write_fd = os.pipe()
    os.set_inheritable(write_fd, True)
    return read_fd, write_fd


def spawn_on_desktop(box: Box, desktop: str, flags: int, script: str, write_fd: int) -> ProcessInformation:
    info = StartupInfo()
    info.cb = ctypes.sizeof(StartupInfo)
    info.lpDesktop = desktop
    info.dwFlags = STARTF_USESTDHANDLES
    info.hStdOutput = info.hStdError = msvcrt.get_osfhandle(write_fd)
    command = subprocess.list2cmdline([str(box.wbsh), "-c", script])
    process = ProcessInformation()
    created = KERNEL32.CreateProcessW(
        str(box.wbsh), ctypes.create_unicode_buffer(command), None, None, True,
        flags | CREATE_UNICODE_ENVIRONMENT, environment_block(box.env()), str(box.root),
        ctypes.byref(info), ctypes.byref(process))
    if not created:
        raise OSError(ctypes.get_last_error(), "CreateProcessW failed")
    return process


def run_on_desktop(box: Box, desktop: str, flags: int, script: str) -> tuple[int, str]:
    read_fd, write_fd = inheritable_pipe()
    try:
        process = spawn_on_desktop(box, desktop, flags, script, write_fd)
    finally:
        os.close(write_fd)
    with os.fdopen(read_fd, "rb") as reader:
        output = reader.read().decode("utf-8", "replace")
    KERNEL32.WaitForSingleObject(process.hProcess, INFINITE)
    code = wintypes.DWORD()
    KERNEL32.GetExitCodeProcess(process.hProcess, ctypes.byref(code))
    KERNEL32.CloseHandle(process.hThread)
    KERNEL32.CloseHandle(process.hProcess)
    return code.value, output


CONSOLE_SCRIPT = """\
"$PROBE_PY" -I "$PROBE_CONSOLE" direct
"$PROBE_PY" -I "$PROBE_CONSOLE" pipeline | cat
echo "$("$PROBE_PY" -I "$PROBE_CONSOLE" substitution)"
"""

CONSOLE_STARTS = (("DETACHED_PROCESS", DETACHED_PROCESS), ("CREATE_NO_WINDOW", CREATE_NO_WINDOW))


def judge_console(start: str, status: int, output: str) -> Optional[str]:
    lines = [line for line in output.splitlines() if ": console=" in line]
    if status != 0 or len(lines) != 3:
        return f"{start}: status {status}, output {output[:200]!r}"
    windowed = [line.split(":")[0] for line in lines if "window=True" in line]
    if windowed:
        return f"{start}: a console window was created for {', '.join(windowed)}"
    return None


def check_no_console(box: Box) -> Outcome:
    name = f"wbsh-check-{os.getpid()}"
    desktop = hidden_desktop(name)
    if not desktop:
        return Outcome("no-console", None, f"skipped: no hidden desktop (error {ctypes.get_last_error()})")
    try:
        problems = [judge_console(start, *run_on_desktop(box, f"WinSta0\\{name}", flags, CONSOLE_SCRIPT))
                    for start, flags in CONSOLE_STARTS]
    finally:
        USER32.CloseDesktop(desktop)
    problems = [problem for problem in problems if problem]
    detail = "; ".join(problems) or "no console program got a window, from either start"
    return Outcome("no-console", not problems, detail)


CHECKS = (check_kill, check_exit, check_no_console)


def run_checks(wbsh: Path) -> list[Outcome]:
    box = make_box(wbsh)
    try:
        return [check(box) for check in CHECKS]
    finally:
        shutil.rmtree(box.root, ignore_errors=True)


def print_outcomes(outcomes: list[Outcome], verbose: bool) -> None:
    for outcome in outcomes:
        state = {True: "ok", False: "FAIL", None: "skip"}[outcome.ok]
        line = f"{outcome.name:<11} {state}"
        if verbose or outcome.ok is not True:
            line += f"  {outcome.detail}"
        print(line)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Check how wbsh's children live and die under an agent host.")
    parser.add_argument("--wbsh", type=Path, default=DEFAULT_WBSH, help="wbsh.exe to test")
    parser.add_argument("--verbose", "-v", action="store_true", help="print details of passing checks too")
    return parser.parse_args()


def main() -> int:
    sys.stdout.reconfigure(errors="backslashreplace")
    args = parse_args()
    if not args.wbsh.is_file():
        print(f"no wbsh at {args.wbsh}", file=sys.stderr)
        return 2
    outcomes = run_checks(args.wbsh)
    print_outcomes(outcomes, args.verbose)
    return 1 if any(outcome.ok is False for outcome in outcomes) else 0


if __name__ == "__main__":
    sys.exit(main())
