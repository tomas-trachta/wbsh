#!/usr/bin/env python3
"""
Agent invocation compatibility harness for wbsh.

Rebuilds, without running any agent CLI, the command lines, environment,
working directory and stdin that coding agents use to start bash on
Windows, and checks that wbsh, copied as bash.exe into a directory with
spaces in its name, answers each of them as GNU bash would.

Shapes (see docs/agents.md for where each one comes from and what is not
verified):

    claude-code  bash -c -l "source SNAPSHOT && eval CMD < /dev/null && pwd -P >| CWDFILE"
    codex        bash -lc CMD
    bash-c       bash -c CMD                     OpenCode, VS Code / Cursor shell tasks
    ec           bash --ec BASE64                any caller that can base64
    script       bash C:\\...\\cmd.sh            long-command fallback
    stdin        bash -s  < CMD                  long-command fallback
    cmd-ec       cmd /c "bash.exe --ec BASE64"   Aider and other shell=True callers
    json         bash --json -c CMD              structured result

Usage:
    python -I tools/agents/check_agent_shapes.py [--wbsh PATH] [--quick]
        [--shape NAME ...] [--verbose]

Exit status: 0 when every check passes, 1 on any failure, 2 on bad usage.
"""

from __future__ import annotations

import argparse
import base64
import concurrent.futures
import ctypes
import importlib.util
import itertools
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Iterator, Optional


REPO = Path(__file__).resolve().parents[2]
DEFAULT_WBSH = REPO / "x64" / "Release" / "wbsh.exe"

CREATE_NO_WINDOW = 0x08000000
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
STILL_ACTIVE = 259

ARGV_LIMIT = 32767
CMD_LINE_LIMIT = 8191
RUN_TIMEOUT = 30.0
HANG_TIMEOUT = 10.0
KILL_GRACE = 5.0

QUICK_EXIT_CODES = (0, 1, 2, 3, 42, 126, 127, 128, 129, 130, 137, 143, 254, 255)

NASTY_STRINGS = (
    "plain",
    "it's \"quoted\"",
    "$HOME $(echo no) `echo no` ${PATH} $((1+1))",
    "back\\slash \\\\ \\\" \\n \\t end",
    "line1\nline2\n\nline4\n",
    "tab\there",
    "\u017elu\u0165ou\u010dk\u00fd k\u016f\u0148 \u65e5\u672c\u8a9e \U0001f389",
    "bang! !! !$ !-1 ^a^b",
    "glob * ? [ab] {a,b} ~ #hash",
    "ops ; & | > < >> && || ( ) \"",
    "%PATH% ^caret %% %~dp0",
    "",
    "-n",
    "--",
    "trailing backslash\\",
    "'",
    "\"",
    "  leading and trailing spaces  ",
)

BASHRC = """\
alias ll='ls -l'
alias say='echo said:'
greet() { printf 'hi %s\\n' "$1"; }
shopt -s extglob
set -o pipefail
"""

SLEEPER = """\
import os, sys, time
with open(sys.argv[1], "w") as f:
    f.write(str(os.getpid()))
time.sleep(120)
"""

ECHO_ARGS = """\
import json, sys
sys.stdout.buffer.write(json.dumps(sys.argv[1:]).encode("ascii"))
"""


def sh_quote(text: str) -> str:
    return "'" + text.replace("'", "'\\''") + "'"


def posix_path(path: Path | str) -> str:
    text = str(path).replace("\\", "/")
    if len(text) >= 2 and text[1] == ":":
        return "/" + text[0].lower() + text[2:]
    return text


def windows_path(text: str) -> str:
    if len(text) >= 3 and text[0] == "/" and text[2] == "/":
        return text[1].upper() + ":" + text[2:].replace("/", "\\")
    return text.replace("/", "\\")


def b64(text: str, encoding: str = "utf-8") -> str:
    return base64.b64encode(text.encode(encoding)).decode("ascii")


NODE_STYLE_NAMES = ("Path", "SystemRoot", "ComSpec", "PathExt", "SystemDrive", "ProgramFiles", "windir")


def node_style_names(env: dict[str, str]) -> dict[str, str]:
    for name in NODE_STYLE_NAMES:
        if name.upper() in env:
            env[name] = env.pop(name.upper())
    return env


@dataclass
class Sandbox:
    root: Path
    bash: Path
    home: Path
    work: Path
    snapshot: Path
    helpers: Path
    serial: Iterator[int] = field(default_factory=itertools.count)

    def fresh(self, stem: str, suffix: str = "") -> Path:
        return self.root / f"{stem}-{next(self.serial)}{suffix}"

    def env(self, extra: Optional[dict[str, str]] = None, node_style: bool = False) -> dict[str, str]:
        env = node_style_names(dict(os.environ)) if node_style else dict(os.environ)
        for name in ("BASH_ENV", "ENV", "SHELLOPTS", "BASHOPTS", "WBSH_LINENO"):
            env.pop(name, None)
        env["HOME"] = str(self.home)
        env["WBSH_PROBE_PY"] = sys.executable
        env["WBSH_PROBE_SLEEPER"] = str(self.helpers / "sleeper.py")
        env["WBSH_PROBE_ECHOARGS"] = str(self.helpers / "echoargs.py")
        env.update(extra or {})
        return env


@dataclass
class Launch:
    argv: list[str] | str
    stdin: Optional[bytes] = None
    env: dict[str, str] = field(default_factory=dict)
    shell: bool = False
    cwd_file: Optional[Path] = None
    node_env: bool = False

    def length(self) -> int:
        if isinstance(self.argv, str):
            return len(self.argv) + len(" /c \"\"") + len(os.environ.get("ComSpec", "cmd.exe"))
        return len(subprocess.list2cmdline(self.argv))


@dataclass
class Run:
    status: Optional[int]
    stdout: bytes = b""
    stderr: bytes = b""
    error: str = ""
    cwd_reported: Optional[str] = None


@dataclass
class Shape:
    key: str
    agents: str
    build: Callable[[Sandbox, str, list[str]], Launch]
    takes_operands: bool = True
    line_limit: int = ARGV_LIMIT
    tracks_cwd: bool = False
    unwrap: Optional[Callable[[Run], Run]] = None


def claude_code_launch(box: Sandbox, script: str, operands: list[str]) -> Launch:
    cwd_file = box.fresh("claude-cwd")
    line = (f"source {sh_quote(posix_path(box.snapshot))} && eval {sh_quote(script)} < /dev/null"
            f" && pwd -P >| {sh_quote(posix_path(cwd_file))}")
    env = {"SHELL": str(box.bash), "CLAUDECODE": "1", "GIT_EDITOR": "true"}
    return Launch([str(box.bash), "-c", "-l", line], env=env, cwd_file=cwd_file, node_env=True)


def codex_launch(box: Sandbox, script: str, operands: list[str]) -> Launch:
    return Launch([str(box.bash), "-lc", script, *operands])


def bash_c_launch(box: Sandbox, script: str, operands: list[str]) -> Launch:
    return Launch([str(box.bash), "-c", script, *operands])


def ec_launch(box: Sandbox, script: str, operands: list[str]) -> Launch:
    return Launch([str(box.bash), "--ec", b64(script), *operands])


def script_launch(box: Sandbox, script: str, operands: list[str]) -> Launch:
    path = box.fresh("cmd", ".sh")
    path.write_bytes(script.encode("utf-8"))
    return Launch([str(box.bash), str(path), *operands[1:]])


def stdin_launch(box: Sandbox, script: str, operands: list[str]) -> Launch:
    return Launch([str(box.bash), "-s", "--", *operands[1:]], stdin=script.encode("utf-8"))


def cmd_ec_launch(box: Sandbox, script: str, operands: list[str]) -> Launch:
    return Launch(f"\"{box.bash}\" --ec {b64(script)}", shell=True)


def json_launch(box: Sandbox, script: str, operands: list[str]) -> Launch:
    return Launch([str(box.bash), "--json", "-c", script, *operands])


def json_unwrap(run: Run) -> Run:
    if run.status is None:
        return run
    try:
        result = json.loads(run.stdout.decode("ascii"))
    except (UnicodeDecodeError, ValueError) as error:
        return Run(None, error=f"--json printed no ASCII JSON object: {error}: {run.stdout[:200]!r}")
    return json_result_run(run, result)


def json_result_run(run: Run, result: dict) -> Run:
    keys = {"status", "stdout", "stderr", "duration_ms"}
    if set(result) != keys or not isinstance(result["duration_ms"], int):
        return Run(None, error=f"--json keys {sorted(result)} are not {sorted(keys)}")
    if run.status != result["status"] or run.stderr:
        mismatch = f"--json exit {run.status} vs status {result['status']}, stderr {run.stderr!r}"
        return Run(None, error=mismatch)
    return Run(result["status"], result["stdout"].encode("utf-8"), result["stderr"].encode("utf-8"))


SHAPES = (
    Shape("claude-code", "Claude Code", claude_code_launch, takes_operands=False, tracks_cwd=True),
    Shape("codex", "Codex CLI", codex_launch),
    Shape("bash-c", "OpenCode, VS Code/Cursor tasks", bash_c_launch),
    Shape("ec", "--ec (any agent)", ec_launch),
    Shape("script", "temp script file", script_launch),
    Shape("stdin", "bash -s < script", stdin_launch),
    Shape("cmd-ec", "Aider (cmd /c) + --ec", cmd_ec_launch, takes_operands=False, line_limit=CMD_LINE_LIMIT),
    Shape("json", "wbsh --json -c", json_launch, unwrap=json_unwrap),
)


def start(box: Sandbox, launch: Launch, cwd: Path, stdin=None) -> subprocess.Popen:
    if stdin is None:
        stdin = subprocess.PIPE if launch.stdin is not None else subprocess.DEVNULL
    env = box.env(launch.env, launch.node_env)
    return subprocess.Popen(launch.argv, shell=launch.shell, cwd=str(cwd), env=env,
                            stdin=stdin, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            creationflags=CREATE_NO_WINDOW)


def read_cwd_file(launch: Launch) -> Optional[str]:
    if launch.cwd_file is None or not launch.cwd_file.exists():
        return None
    return launch.cwd_file.read_bytes().decode("utf-8", "replace").rstrip("\n")


def execute(box: Sandbox, launch: Launch, cwd: Path) -> Run:
    try:
        process = start(box, launch, cwd)
    except OSError as error:
        return Run(None, error=f"spawn failed: {error}")
    try:
        stdout, stderr = process.communicate(launch.stdin, timeout=RUN_TIMEOUT)
    except subprocess.TimeoutExpired:
        kill_tree(process.pid)
        process.communicate()
        return Run(None, error=f"timed out after {RUN_TIMEOUT:.0f}s")
    return Run(process.returncode, stdout, stderr, cwd_reported=read_cwd_file(launch))


def run_launch(box: Sandbox, shape: Shape, launch: Launch, cwd: Optional[Path] = None) -> Run:
    run = execute(box, launch, cwd or box.work)
    return shape.unwrap(run) if shape.unwrap else run


def run_shape(box: Sandbox, shape: Shape, script: str, operands: Optional[list[str]] = None,
              cwd: Optional[Path] = None) -> Run:
    return run_launch(box, shape, shape.build(box, script, operands or []), cwd)


@dataclass
class Outcome:
    state: str
    detail: str = ""


PASS = Outcome("ok")
NOT_APPLICABLE = Outcome("-")


def fail(detail: str) -> Outcome:
    return Outcome("FAIL", detail)


def describe(run: Run) -> str:
    if run.status is None:
        return run.error
    return f"status {run.status}, stdout {run.stdout[:160]!r}, stderr {run.stderr[:160]!r}"


def expect(run: Run, status: int, stdout: bytes, stderr: Optional[bytes] = b"") -> Optional[str]:
    if run.status != status or run.stdout != stdout:
        return f"want status {status} stdout {stdout[:80]!r}; got {describe(run)}"
    if stderr is not None and run.stderr != stderr:
        return f"want stderr {stderr[:80]!r}; got {describe(run)}"
    return None


def first_failure(results: list[Optional[str]]) -> Outcome:
    failures = [result for result in results if result]
    if not failures:
        return PASS
    return fail(f"{len(failures)} failed; first: {failures[0]}")


def check_exit_codes(box: Sandbox, shape: Shape, quick: bool) -> Outcome:
    codes = QUICK_EXIT_CODES if quick else range(256)
    return first_failure([expect(run_shape(box, shape, f"exit {code}"), code, b"") for code in codes])


def check_last_status(box: Sandbox, shape: Shape, quick: bool) -> Outcome:
    cases = (("(exit 93)", 93), ("false", 1), ("f() { return 7; }; f", 7), ("no_such_cmd_x 2>/dev/null", 127),
             ("exit 300", 44), ("trap 'exit 5' EXIT; true", 5))
    return first_failure([expect(run_shape(box, shape, script), status, b"") for script, status in cases])


def check_streams(box: Sandbox, shape: Shape, quick: bool) -> Outcome:
    run = run_shape(box, shape, "echo out; echo err >&2; printf tail; printf ' e2' 1>&2")
    return first_failure([expect(run, 0, b"out\ntail", b"err\n e2")])


def check_quiet(box: Sandbox, shape: Shape, quick: bool) -> Outcome:
    return first_failure([expect(run_shape(box, shape, "true"), 0, b"")])


def check_order(box: Sandbox, shape: Shape, quick: bool) -> Outcome:
    script = ('echo 1; "$WBSH_PROBE_PY" -I -c \'import sys; sys.stdout.buffer.write(b"2\\n")\'; echo 3;'
              " printf 'b\\na\\n' | sort | head -n 1")
    return first_failure([expect(run_shape(box, shape, script), 0, b"1\n2\n3\na\n")])


def check_stdin_open(box: Sandbox, shape: Shape, quick: bool) -> Outcome:
    if shape.key == "stdin":
        return NOT_APPLICABLE
    launch = shape.build(box, "echo ok", [])
    process = start(box, launch, box.work, stdin=subprocess.PIPE)
    try:
        process.wait(timeout=HANG_TIMEOUT)
    except subprocess.TimeoutExpired:
        kill_tree(process.pid)
        return fail(f"no exit within {HANG_TIMEOUT:.0f}s while stdin stays open")
    finally:
        process.communicate()
    return PASS


def check_cwd(box: Sandbox, shape: Shape, quick: bool) -> Outcome:
    run = run_shape(box, shape, "pwd")
    return first_failure([expect(run, 0, (posix_path(box.work) + "\n").encode("utf-8"))])


def short_path(path: Path) -> Optional[Path]:
    buffer = ctypes.create_unicode_buffer(1024)
    if not ctypes.windll.kernel32.GetShortPathNameW(str(path), buffer, len(buffer)):
        return None
    return Path(buffer.value) if buffer.value != str(path) else None


def check_cwd_short(box: Sandbox, shape: Shape, quick: bool) -> Outcome:
    short = short_path(box.work)
    if short is None:
        return Outcome("-", "no 8.3 names on this volume")
    run = run_shape(box, shape, "pwd", cwd=short)
    return first_failure([expect(run, 0, (posix_path(box.work) + "\n").encode("utf-8"))])


def check_nested_bash(box: Sandbox, shape: Shape, quick: bool) -> Outcome:
    run = run_shape(box, shape, """"$BASH" -c 'printf "nested %s" "$1"' _ 'a b'""")
    return first_failure([expect(run, 0, b"nested a b")])


def check_bare_stdin(box: Sandbox, shape: Shape, quick: bool) -> Outcome:
    if shape.key != "stdin":
        return NOT_APPLICABLE
    run = execute(box, Launch([str(box.bash)], stdin=b"echo hi; echo err >&2\nexit 4\n"), box.work)
    return first_failure([expect(run, 4, b"hi\n", b"err\n")])


def check_cwd_tracking(box: Sandbox, shape: Shape, quick: bool) -> Outcome:
    if not shape.tracks_cwd:
        return NOT_APPLICABLE
    launch = shape.build(box, "cd sub", [])
    run = execute(box, launch, box.work)
    want = posix_path(box.work / "sub")
    if run.status != 0 or run.cwd_reported != want:
        return fail(f"cwd file {run.cwd_reported!r}, want {want!r}; {describe(run)}")
    if Path(windows_path(run.cwd_reported)) != box.work / "sub":
        return fail(f"{run.cwd_reported!r} does not map back to {box.work / 'sub'}")
    return PASS


def check_env(box: Sandbox, shape: Shape, quick: bool) -> Outcome:
    value = "a b'c\"$d\\e!f %PATH% \u017e\u65e5"
    launch = shape.build(box, 'printf %s "$WBSH_PROBE_VALUE"', [])
    launch.env["WBSH_PROBE_VALUE"] = value
    return first_failure([expect(run_launch(box, shape, launch), 0, value.encode("utf-8"))])


def check_quoting(box: Sandbox, shape: Shape, quick: bool) -> Outcome:
    results = []
    for text in NASTY_STRINGS:
        run = run_shape(box, shape, f"printf %s {sh_quote(text)}")
        results.append(expect(run, 0, text.encode("utf-8")))
    return first_failure(results)


def check_operands(box: Sandbox, shape: Shape, quick: bool) -> Outcome:
    if not shape.takes_operands:
        return NOT_APPLICABLE
    results = []
    for text in NASTY_STRINGS:
        run = run_shape(box, shape, 'printf %s "$1"', ["name", text])
        results.append(expect(run, 0, text.encode("utf-8")))
    return first_failure(results)


def check_native_argv(box: Sandbox, shape: Shape, quick: bool) -> Outcome:
    words = " ".join(sh_quote(text) for text in NASTY_STRINGS)
    run = run_shape(box, shape, f'"$WBSH_PROBE_PY" -I "$WBSH_PROBE_ECHOARGS" {words}')
    want = json.dumps(list(NASTY_STRINGS)).encode("ascii")
    return first_failure([expect(run, 0, want)])


def long_script(size: int) -> tuple[str, bytes]:
    lines, out, length, index = [], [], 0, 0
    while length < size:
        lines.append(f"echo L{index:05d}")
        out.append(f"L{index:05d}\n")
        length += len(lines[-1]) + 1
        index += 1
    return "\n".join(lines) + "\n", "".join(out).encode("ascii")


def check_long(box: Sandbox, shape: Shape, size: int) -> Outcome:
    script, want = long_script(size)
    launch = shape.build(box, script, [])
    if launch.length() > shape.line_limit:
        return Outcome("limit", f"{launch.length()} chars > {shape.line_limit}; use a script file or -s")
    return first_failure([expect(run_launch(box, shape, launch), 0, want)])


def check_long_9k(box: Sandbox, shape: Shape, quick: bool) -> Outcome:
    return check_long(box, shape, 9 * 1024)


def check_long_40k(box: Sandbox, shape: Shape, quick: bool) -> Outcome:
    return check_long(box, shape, 40 * 1024)


def process_alive(pid: int) -> bool:
    kernel32 = ctypes.windll.kernel32
    handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return False
    code = ctypes.c_ulong()
    kernel32.GetExitCodeProcess(handle, ctypes.byref(code))
    kernel32.CloseHandle(handle)
    return code.value == STILL_ACTIVE


def kill_tree(pid: int) -> None:
    subprocess.run(["taskkill", "/T", "/F", "/PID", str(pid)], stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, creationflags=CREATE_NO_WINDOW)


def wait_until(predicate: Callable[[], bool], seconds: float) -> bool:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.05)
    return predicate()


def start_sleeper(box: Sandbox, shape: Shape) -> tuple[subprocess.Popen, Path]:
    pid_file = box.fresh("sleeper", ".pid")
    script = f'"$WBSH_PROBE_PY" -I "$WBSH_PROBE_SLEEPER" {sh_quote(str(pid_file))}; echo after'
    launch = shape.build(box, script, [])
    process = start(box, launch, box.work)
    if launch.stdin is not None:
        process.stdin.write(launch.stdin)
        process.stdin.close()
    return process, pid_file


def sleeper_pid(pid_file: Path) -> Optional[int]:
    if not wait_until(lambda: pid_file.exists() and pid_file.read_text().strip() != "", RUN_TIMEOUT):
        return None
    return int(pid_file.read_text().strip())


def check_kill_tree(box: Sandbox, shape: Shape, quick: bool) -> Outcome:
    process, pid_file = start_sleeper(box, shape)
    grandchild = sleeper_pid(pid_file)
    kill_tree(process.pid)
    process.communicate()
    if grandchild is None:
        return fail("the external grandchild never started")
    if not wait_until(lambda: not process_alive(grandchild), KILL_GRACE):
        kill_tree(grandchild)
        return fail("taskkill /T left the grandchild running")
    return PASS


# Node's child.kill() and Python's Popen.kill() end only the shell: the
# command it waits for has to end with it, or it holds the pipes open.
def check_kill_shell_only(box: Sandbox, shape: Shape, quick: bool) -> Outcome:
    if shape.key not in ("bash-c", "claude-code"):
        return NOT_APPLICABLE
    process, pid_file = start_sleeper(box, shape)
    grandchild = sleeper_pid(pid_file)
    process.kill()
    if grandchild is None:
        process.communicate()
        return fail("the external grandchild never started")
    ended = wait_until(lambda: not process_alive(grandchild), KILL_GRACE)
    try:
        process.communicate(timeout=KILL_GRACE)
        closed = True
    except subprocess.TimeoutExpired:
        closed = False
    kill_tree(grandchild)
    process.communicate()
    if ended and closed:
        return PASS
    return fail(f"grandchild {'ends' if ended else 'survives'}, pipes {'closed' if closed else 'held open'}")


def check_ec_utf16(box: Sandbox, shape: Shape, quick: bool) -> Outcome:
    if shape.key != "ec":
        return NOT_APPLICABLE
    text = NASTY_STRINGS[6]
    script = f"printf %s {sh_quote(text)}"
    launch = Launch([str(box.bash), "--ec", b64(script, "utf-16-le")])
    return first_failure([expect(execute(box, launch, box.work), 0, text.encode("utf-8"))])


def check_snapshot(box: Sandbox, shape: Shape, quick: bool) -> Outcome:
    if shape.key != "claude-code":
        return NOT_APPLICABLE
    script = ("greet x; say y; shopt -q extglob && echo extglob;"
              " [[ abc == @(a|x)bc ]] && echo match; [[ -o pipefail ]] && echo pipefail;"
              " command -v cmd >/dev/null && echo path")
    want = b"hi x\nsaid: y\nextglob\nmatch\npipefail\npath\n"
    return first_failure([expect(run_shape(box, shape, script), 0, want)])


def check_json_binary(box: Sandbox, shape: Shape, quick: bool) -> Outcome:
    if shape.key != "json":
        return NOT_APPLICABLE
    script = "printf 'a\\377b\\0c\\001'; printf '\\342\\202' >&2; echo -n \U0001f389 >&2"
    run = run_shape(box, shape, script)
    return first_failure([expect(run, 0, "a\ufffdb\x00c\x01".encode(), "\ufffd\ufffd\U0001f389".encode())])


def check_json_ec(box: Sandbox, shape: Shape, quick: bool) -> Outcome:
    if shape.key != "json":
        return NOT_APPLICABLE
    launch = Launch([str(box.bash), "--json", "--ec", b64("echo hi; echo oops >&2; exit 3")])
    return first_failure([expect(json_unwrap(execute(box, launch, box.work)), 3, b"hi\n", b"oops\n")])


CHECKS: tuple[tuple[str, Callable[[Sandbox, Shape, bool], Outcome]], ...] = (
    ("exit 0..255", check_exit_codes),
    ("last status, exit 300, EXIT trap", check_last_status),
    ("stdout/stderr separate", check_streams),
    ("silent `true` (no banner/prompt)", check_quiet),
    ("output order, native + pipeline", check_order),
    ("stdin left open: no hang", check_stdin_open),
    ("cwd (spaces, unicode)", check_cwd),
    ("cwd given as 8.3 short path", check_cwd_short),
    ("cwd tracking file (pwd -P)", check_cwd_tracking),
    ("env passing", check_env),
    ('nested "$BASH" -c', check_nested_bash),
    ("quoting round-trip", check_quoting),
    ("argv operands round-trip", check_operands),
    ("argv to native exe", check_native_argv),
    ("9 KB command", check_long_9k),
    ("40 KB command", check_long_40k),
    ("kill tree (taskkill /T)", check_kill_tree),
    ("kill shell only", check_kill_shell_only),
    ("--ec UTF-16LE", check_ec_utf16),
    ("bare `bash` reading a pipe", check_bare_stdin),
    ("snapshot: alias/function/opts", check_snapshot),
    ("--json invalid UTF-8, NUL", check_json_binary),
    ("--json --ec", check_json_ec),
)


def snapshot_script(snapshot: Path) -> str:
    return f"""\
SNAPSHOT_FILE={sh_quote(posix_path(snapshot))}
source ~/.bashrc < /dev/null
echo "# Snapshot file" >| "$SNAPSHOT_FILE"
echo "unalias -a 2>/dev/null || true" >> "$SNAPSHOT_FILE"
declare -F | cut -d' ' -f3 | grep -vE '^(_|__)' | while read func; do
    encoded_func=$(declare -f "$func" | base64)
    echo "eval \\"\\$(echo '$encoded_func' | base64 -d)\\" > /dev/null 2>&1" >> "$SNAPSHOT_FILE"
done
shopt -p | head -n 1000 >> "$SNAPSHOT_FILE"
set -o | grep "on" | awk '{{print "set -o " $1}}' | head -n 1000 >> "$SNAPSHOT_FILE"
echo "shopt -s expand_aliases" >> "$SNAPSHOT_FILE"
alias | sed 's/^alias //g' | sed 's/^/alias -- /' | head -n 1000 >> "$SNAPSHOT_FILE"
echo "export PATH=$(printf %q "$PATH")" >> "$SNAPSHOT_FILE"
"""


def make_snapshot(box: Sandbox) -> Optional[str]:
    launch = Launch([str(box.bash), "-c", "-l", snapshot_script(box.snapshot)])
    run = execute(box, launch, box.home)
    if run.status != 0 or run.stderr or not box.snapshot.exists():
        return f"creating the snapshot: {describe(run)}"
    check = execute(box, Launch([str(box.bash), "-c", "-l", f"source {sh_quote(posix_path(box.snapshot))}"]),
                    box.work)
    if check.status != 0 or check.stdout or check.stderr:
        return f"sourcing the snapshot: {describe(check)}"
    return None


def long_path(path: Path) -> Path:
    buffer = ctypes.create_unicode_buffer(1024)
    if not ctypes.windll.kernel32.GetLongPathNameW(str(path), buffer, len(buffer)):
        return path
    return Path(buffer.value)


# TEMP can be an 8.3 path (C:\Users\RUNNER~1 on CI), and wbsh's pwd answers
# with the long name, which is what the cwd checks expect.
def make_sandbox(wbsh: Path) -> Sandbox:
    root = long_path(Path(tempfile.mkdtemp(prefix="wbsh-agents-")))
    bin_dir = root / "Program Files" / "Git" / "bin"
    work = root / "work dir \u017e \u65e5\u672c"
    helpers = root / "helpers"
    for directory in (bin_dir, root / "home", work / "sub", helpers):
        directory.mkdir(parents=True)
    shutil.copy2(wbsh, bin_dir / "bash.exe")
    (root / "home" / ".bashrc").write_text(BASHRC, encoding="utf-8", newline="\n")
    (helpers / "sleeper.py").write_text(SLEEPER, encoding="utf-8", newline="\n")
    (helpers / "echoargs.py").write_text(ECHO_ARGS, encoding="utf-8", newline="\n")
    return Sandbox(root, bin_dir / "bash.exe", root / "home", work, root / "snapshot-bash.sh", helpers)


def run_matrix(box: Sandbox, shapes: list[Shape], quick: bool) -> dict[tuple[str, str], Outcome]:
    jobs = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=min(8, os.cpu_count() or 4)) as pool:
        for name, check in CHECKS:
            for shape in shapes:
                jobs[(name, shape.key)] = pool.submit(check, box, shape, quick)
    return {key: future.result() for key, future in jobs.items()}


def print_table(shapes: list[Shape], results: dict[tuple[str, str], Outcome]) -> None:
    name_width = max(len(name) for name, _ in CHECKS)
    widths = [max(len(shape.key), 5) for shape in shapes]
    print(" " * name_width + "  " + "  ".join(shape.key.ljust(w) for shape, w in zip(shapes, widths)))
    for name, _ in CHECKS:
        cells = (results[(name, shape.key)].state.ljust(w) for shape, w in zip(shapes, widths))
        print(name.ljust(name_width) + "  " + "  ".join(cells))


def print_notes(shapes: list[Shape], results: dict[tuple[str, str], Outcome], verbose: bool) -> None:
    print()
    for shape in shapes:
        print(f"  {shape.key:<12} {shape.agents}")
    for (name, key), outcome in results.items():
        if outcome.detail and (verbose or outcome.state in ("FAIL", "info", "limit")):
            print(f"\n[{outcome.state}] {name} / {key}:\n    {outcome.detail}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Check wbsh against coding agents' bash invocations.")
    parser.add_argument("--wbsh", type=Path, default=DEFAULT_WBSH, help="wbsh.exe to test")
    parser.add_argument("--quick", action="store_true", help="sample the exit codes instead of all 256")
    parser.add_argument("--shape", action="append", choices=[shape.key for shape in SHAPES],
                        help="only these shapes (repeatable)")
    parser.add_argument("--keep", action="store_true", help="keep the sandbox directory")
    parser.add_argument("--verbose", "-v", action="store_true", help="print details of passing checks too")
    return parser.parse_args()


def main() -> int:
    sys.stdout.reconfigure(errors="backslashreplace")
    args = parse_args()
    if not args.wbsh.is_file():
        print(f"no wbsh at {args.wbsh}", file=sys.stderr)
        return 2
    shapes = [shape for shape in SHAPES if not args.shape or shape.key in args.shape]
    box = make_sandbox(args.wbsh)
    try:
        return run_all(box, shapes, args)
    finally:
        if args.keep:
            print(f"\nsandbox kept: {box.root}")
        else:
            shutil.rmtree(box.root, ignore_errors=True)


def load_lifetime_checks():
    path = Path(__file__).with_name("check_process_lifetime.py")
    spec = importlib.util.spec_from_file_location("check_process_lifetime", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def run_lifetime_checks(wbsh: Path, verbose: bool) -> int:
    lifetime = load_lifetime_checks()
    print("\nprocess lifetime and console (check_process_lifetime.py):")
    outcomes = lifetime.run_checks(wbsh)
    lifetime.print_outcomes(outcomes, verbose)
    return sum(outcome.ok is False for outcome in outcomes)


def run_all(box: Sandbox, shapes: list[Shape], args: argparse.Namespace) -> int:
    started = time.monotonic()
    snapshot_error = make_snapshot(box)
    if snapshot_error:
        print(f"FAIL claude-code snapshot: {snapshot_error}")
    results = run_matrix(box, shapes, args.quick)
    print_table(shapes, results)
    print_notes(shapes, results, args.verbose)
    failed = sum(outcome.state == "FAIL" for outcome in results.values()) + bool(snapshot_error)
    failed += run_lifetime_checks(args.wbsh, args.verbose)
    print(f"\n{failed} failure(s), {time.monotonic() - started:.0f}s, wbsh {args.wbsh}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
