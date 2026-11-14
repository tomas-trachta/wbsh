#!/usr/bin/env python3
"""
Benchmark harness: times a fixed set of shell workloads under one or more
shells, hyperfine-style (warm-up runs, then N timed runs with the shells
interleaved so drift in machine load hits every shell alike).

Usage:
    python tools/bench/bench.py --shell wbsh=x64/Release/wbsh.exe \\
        --shell "git-bash=C:/Program Files/Git/bin/bash.exe" \\
        --runs 10 --json bench.json --markdown bench.md

The first --shell is the baseline the other columns are compared with.
See README.md for what each case measures.

Perf gate (see README.md, "Perf gate"):
    python tools/bench/bench.py --shell wbsh=x64/Release/wbsh.exe \\
        --shell "git-bash=C:/Program Files/Git/bin/bash.exe" --yardstick git-bash \\
        --gate tools/bench/baselines/local.json
    python tools/bench/bench.py ... --yardstick git-bash --write-baseline tools/bench/baselines/local.json
    python tools/bench/bench.py --from-json bench.json --yardstick git-bash --gate baseline.json

Exit status: 0, or 1 when --gate finds a case that regressed beyond --threshold.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import os
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path


WINDOWS_BELOW_NORMAL = 0x00004000
READ_LOOP_LINES = 5000
DEFAULT_THRESHOLD = 0.20
DEFAULT_MIN_DELTA_MS = 3.0
BASELINE_FORMAT = 1


@dataclass
class Shell:
    name: str
    path: str
    version: str = ""


@dataclass
class Bench:
    name: str
    description: str
    script: str | None = None
    arguments: list[str] = field(default_factory=list)


@dataclass
class Verdict:
    case: str
    baseline: float
    current: float
    unit: str
    regressed: bool = False

    @property
    def change(self) -> float:
        return self.current / self.baseline - 1.0 if self.baseline else 0.0


@dataclass
class Measurement:
    shell: str
    case: str
    seconds: list[float] = field(default_factory=list)
    failures: int = 0

    def stats(self) -> dict:
        if not self.seconds:
            return {"runs": 0, "failures": self.failures}
        ordered = sorted(self.seconds)
        return {
            "runs": len(ordered),
            "failures": self.failures,
            "min": ordered[0],
            "p50": percentile(ordered, 50),
            "p90": percentile(ordered, 90),
            "mean": statistics.fmean(ordered),
            "stdev": statistics.stdev(ordered) if len(ordered) > 1 else 0.0,
        }


def percentile(ordered: list[float], percent: float) -> float:
    if len(ordered) == 1:
        return ordered[0]
    rank = (len(ordered) - 1) * percent / 100.0
    low = int(rank)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (rank - low)


def native_spawn_target() -> str:
    system_root = os.environ.get("SystemRoot", r"C:\Windows")
    return str(Path(system_root) / "System32" / "hostname.exe").replace("\\", "/")


def build_cases(scale: float) -> list[Bench]:
    def count(base: int) -> int:
        return max(1, int(base * scale))

    return [
        Bench("startup", "`-c true`: process start to exit", arguments=["-c", "true"]),
        Bench(
            "function_calls", f"{count(20000)} calls of an empty function",
            f"f() {{ :; }}\nfor ((i = 0; i < {count(20000)}; i++)); do f; done\n",
        ),
        Bench(
            "arith_loop", f"{count(50000)} iterations of `(( x += i ))`",
            f"x=0\nfor ((i = 0; i < {count(50000)}; i++)); do (( x += i )); done\necho $x\n",
        ),
        Bench(
            "string_append", f"{count(20000)} appends of one character",
            f"s=\nfor ((i = 0; i < {count(20000)}; i++)); do s+=x; done\necho ${{#s}}\n",
        ),
        Bench(
            "array_fill", f"{count(20000)} appends to an indexed array",
            f"a=()\nfor ((i = 0; i < {count(20000)}; i++)); do a+=(\"$i\"); done\necho ${{#a[@]}}\n",
        ),
        Bench(
            "builtin_pipeline", f"{count(25)} runs of `echo | tr | cut`",
            f"for ((i = 0; i < {count(25)}; i++)); do\n"
            f"    echo 'alpha beta gamma' | tr a-z A-Z | cut -d ' ' -f 2 > /dev/null\n"
            f"done\n",
        ),
        Bench(
            "external_spawn", f"{count(150)} runs of System32\\hostname.exe",
            f"for ((i = 0; i < {count(150)}; i++)); do '{native_spawn_target()}' > /dev/null; done\n",
        ),
        Bench(
            "cmdsubst_loop", f"{count(100)} command substitutions `$(echo $i)`",
            f"for ((i = 0; i < {count(100)}; i++)); do x=$(echo $i); done\necho $x\n",
        ),
        Bench(
            "read_loop", f"`while read` over a {READ_LOOP_LINES}-line file",
            "n=0\nwhile IFS= read -r line; do n=$((n + 1)); done < lines.txt\necho $n\n",
        ),
    ]


def prepare_workdir(directory: Path, cases: list[Bench]) -> None:
    lines = "".join(f"line {i} with some text\n" for i in range(READ_LOOP_LINES))
    (directory / "lines.txt").write_bytes(lines.encode("ascii"))
    for case in cases:
        if case.script is not None:
            (directory / f"{case.name}.sh").write_bytes(case.script.encode("ascii"))


def command_for(shell: Shell, case: Bench) -> list[str]:
    if case.script is None:
        return [shell.path, *case.arguments]
    return [shell.path, f"{case.name}.sh"]


def creation_flags(low_priority: bool) -> int:
    if os.name == "nt" and low_priority:
        return WINDOWS_BELOW_NORMAL
    return 0


def time_once(command: list[str], workdir: Path, low_priority: bool) -> float | None:
    started = time.perf_counter()
    completed = subprocess.run(
        command, cwd=workdir, stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        creationflags=creation_flags(low_priority),
    )
    elapsed = time.perf_counter() - started
    return elapsed if completed.returncode == 0 else None


def warm_up(shells, cases, workdir: Path, options) -> None:
    for _ in range(options.warmup):
        for case in cases:
            for shell in shells:
                time_once(command_for(shell, case), workdir, options.low_priority)


def measure(shells, cases, workdir: Path, options) -> dict[tuple[str, str], Measurement]:
    results = {(shell.name, case.name): Measurement(shell.name, case.name) for shell in shells for case in cases}
    for run in range(options.runs):
        for case in cases:
            for shell in rotated(shells, run):
                elapsed = time_once(command_for(shell, case), workdir, options.low_priority)
                record(results[(shell.name, case.name)], elapsed)
        print(f"run {run + 1}/{options.runs} done", file=sys.stderr)
    return results


def rotated(items: list, offset: int) -> list:
    offset %= len(items)
    return items[offset:] + items[:offset]


def record(measurement: Measurement, elapsed: float | None) -> None:
    if elapsed is None:
        measurement.failures += 1
    else:
        measurement.seconds.append(elapsed)


def shell_version(path: str) -> str:
    try:
        completed = subprocess.run(
            [path, "-c", 'printf %s "$BASH_VERSION"'],
            stdin=subprocess.DEVNULL, capture_output=True, timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired):
        return "?"
    return completed.stdout.decode("utf-8", errors="replace").strip() or "?"


def windows_cpu_name() -> str:
    import winreg
    key_path = r"HARDWARE\DESCRIPTION\System\CentralProcessor\0"
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, key_path) as key:
        return str(winreg.QueryValueEx(key, "ProcessorNameString")[0]).strip()


def linux_cpu_name() -> str:
    for line in Path("/proc/cpuinfo").read_text().splitlines():
        if line.startswith("model name"):
            return line.split(":", 1)[1].strip()
    return platform.processor()


def windows_memory_gib() -> float:
    import ctypes

    class MemoryStatus(ctypes.Structure):
        _fields_ = [
            ("length", ctypes.c_ulong), ("load", ctypes.c_ulong),
            ("total_physical", ctypes.c_ulonglong), ("available_physical", ctypes.c_ulonglong),
            ("total_page", ctypes.c_ulonglong), ("available_page", ctypes.c_ulonglong),
            ("total_virtual", ctypes.c_ulonglong), ("available_virtual", ctypes.c_ulonglong),
            ("available_extended", ctypes.c_ulonglong),
        ]

    status = MemoryStatus()
    status.length = ctypes.sizeof(MemoryStatus)
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status))
    return status.total_physical / 2 ** 30


def machine_description() -> dict:
    description = {
        "os": platform.platform(),
        "logical_cpus": os.cpu_count(),
        "python": platform.python_version(),
    }
    if os.name == "nt":
        description["cpu"] = windows_cpu_name()
        description["memory_gib"] = round(windows_memory_gib(), 1)
        description["windows_build"] = platform.version()
    elif Path("/proc/cpuinfo").exists():
        description["cpu"] = linux_cpu_name()
    return description


def format_ms(seconds: float) -> str:
    milliseconds = seconds * 1000
    return f"{milliseconds:.0f} ms" if milliseconds >= 10 else f"{milliseconds:.1f} ms"


def markdown_cell(stats: dict, baseline: dict | None) -> str:
    if not stats["runs"]:
        return f"failed ({stats['failures']} runs)"
    cell = f"{format_ms(stats['p50'])} <sub>p90 {format_ms(stats['p90'])}</sub>"
    if baseline and baseline.get("runs"):
        cell += f" ({stats['p50'] / baseline['p50']:.2f}x)"
    if stats["failures"]:
        cell += f" ({stats['failures']} failed)"
    return cell


def markdown_table(shells, cases, results) -> str:
    header = "| case | " + " | ".join(shell.name for shell in shells) + " |"
    divider = "| --- | " + " | ".join("---:" for _ in shells) + " |"
    rows = [header, divider]
    for case in cases:
        baseline = results[(shells[0].name, case.name)].stats()
        cells = [markdown_cell(baseline, None)]
        cells += [markdown_cell(results[(shell.name, case.name)].stats(), baseline) for shell in shells[1:]]
        rows.append(f"| `{case.name}` | " + " | ".join(cells) + " |")
    return "\n".join(rows)


def markdown_escape(text: str) -> str:
    return text.replace("|", "\\|")


def markdown_report(shells, cases, results, machine, options) -> str:
    lines = [
        "| shell | version | path |",
        "| --- | --- | --- |",
        *[f"| {shell.name} | `{shell.version}` | `{shell.path}` |" for shell in shells],
        "",
        f"{options.runs} timed runs per cell after {options.warmup} warm-up run(s), shells interleaved; "
        f"each cell is the median wall time with the 90th percentile beside it, "
        f"and the ratio to `{shells[0].name}` in brackets.",
        "",
        markdown_table(shells, cases, results),
        "",
        "| case | workload |",
        "| --- | --- |",
        *[f"| `{case.name}` | {markdown_escape(case.description)} |" for case in cases],
        "",
        "Machine: " + ", ".join(f"{key} {value}" for key, value in machine.items()),
    ]
    return "\n".join(lines) + "\n"


def json_report(shells, cases, results, machine, options) -> dict:
    return {
        "recorded": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "machine": machine,
        "protocol": {"runs": options.runs, "warmup": options.warmup, "scale": options.scale},
        "shells": [{"name": shell.name, "path": shell.path, "version": shell.version} for shell in shells],
        "cases": [{"name": case.name, "description": case.description} for case in cases],
        "results": [
            {"shell": shell.name, "case": case.name, **results[(shell.name, case.name)].stats(),
             "samples": results[(shell.name, case.name)].seconds}
            for case in cases for shell in shells
        ],
    }


def parse_shell(spec: str) -> Shell:
    name, separator, path = spec.partition("=")
    if not separator or not name or not path:
        raise argparse.ArgumentTypeError(f"expected NAME=PATH, got {spec!r}")
    resolved = str(Path(path).resolve()) if Path(path).is_file() else shutil.which(path)
    if not resolved:
        raise argparse.ArgumentTypeError(f"shell not found: {path}")
    return Shell(name, resolved)


def lower_own_priority() -> None:
    if os.name == "nt":
        import ctypes
        kernel32 = ctypes.windll.kernel32
        kernel32.SetPriorityClass(kernel32.GetCurrentProcess(), WINDOWS_BELOW_NORMAL)
    else:
        os.nice(10)


def p50_table(results, shells, cases) -> dict[str, dict[str, float]]:
    table = {}
    for case in cases:
        row = {}
        for shell in shells:
            stats = results[(shell.name, case.name)].stats()
            if stats["runs"]:
                row[shell.name] = stats["p50"]
        table[case.name] = row
    return table


def p50_table_from_json(document: dict) -> dict[str, dict[str, float]]:
    table: dict[str, dict[str, float]] = {}
    for entry in document["results"]:
        if entry.get("runs"):
            table.setdefault(entry["case"], {})[entry["shell"]] = entry["p50"]
    return table


def case_metric(row: dict[str, float], subject: str, yardstick: str | None) -> float | None:
    if subject not in row:
        return None
    if yardstick is None:
        return row[subject]
    if not row.get(yardstick):
        return None
    return row[subject] / row[yardstick]


def baseline_document(table, subject: Shell, yardstick: Shell | None, options) -> dict:
    cases = {}
    for case, row in table.items():
        metric = case_metric(row, subject.name, yardstick.name if yardstick else None)
        if metric is not None:
            cases[case] = {"metric": metric, "subject_p50": row[subject.name],
                           "yardstick_p50": row.get(yardstick.name) if yardstick else None}
    return {
        "format": BASELINE_FORMAT,
        "recorded": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "machine": machine_description(),
        "protocol": {"runs": options.runs, "warmup": options.warmup, "scale": options.scale},
        "subject": {"name": subject.name, "version": subject.version},
        "yardstick": {"name": yardstick.name, "version": yardstick.version} if yardstick else None,
        "cases": cases,
    }


def regressed(verdict: Verdict, delta_ms: float, options) -> bool:
    return verdict.change > options.threshold and delta_ms > options.min_delta_ms


def judge_case(case: str, row: dict[str, float], entry: dict, subject: str, yardstick: str | None, options):
    current = case_metric(row, subject, yardstick)
    if current is None:
        return None
    unit = f"x {yardstick}" if yardstick else "s"
    verdict = Verdict(case, entry["metric"], current, unit)
    scale = row[yardstick] if yardstick else 1.0
    delta_ms = (row[subject] - entry["metric"] * scale) * 1000
    verdict.regressed = regressed(verdict, delta_ms, options)
    return verdict


def compare_with_baseline(table, baseline: dict, subject: str, yardstick: str | None, options) -> list[Verdict]:
    verdicts = []
    for case, entry in baseline["cases"].items():
        verdict = judge_case(case, table.get(case, {}), entry, subject, yardstick, options)
        if verdict is not None:
            verdicts.append(verdict)
    return verdicts


def merge_best(first: dict, second: dict, subject: str, yardstick: str | None) -> dict:
    merged = dict(first)
    for case, row in second.items():
        old = case_metric(first.get(case, {}), subject, yardstick)
        new = case_metric(row, subject, yardstick)
        if new is not None and (old is None or new < old):
            merged[case] = row
    return merged


def remeasure(shells, cases, case_names: set[str], options) -> dict:
    retry = [case for case in cases if case.name in case_names]
    with tempfile.TemporaryDirectory(prefix="wbsh-bench-") as directory:
        workdir = Path(directory)
        prepare_workdir(workdir, retry)
        warm_up(shells, retry, workdir, options)
        results = measure(shells, retry, workdir, options)
    return p50_table(results, shells, retry)


def yardstick_mismatch(baseline: dict, versions: dict[str, str]) -> str | None:
    recorded = baseline.get("yardstick")
    if not recorded:
        return None
    current = versions.get(recorded["name"])
    if current is None:
        return f"the baseline was normalized by `{recorded['name']}`, which is not part of this run"
    if current != recorded["version"]:
        return f"`{recorded['name']}` is `{current}` here but `{recorded['version']}` in the baseline"
    return None


def format_metric(value: float, unit: str) -> str:
    return f"{value * 1000:.1f} ms" if unit == "s" else f"{value:.3f} {unit}"


def gate_report(verdicts: list[Verdict], baseline_path: str, warning: str | None, options) -> str:
    lines = [
        "## Perf gate",
        "",
        f"Baseline `{baseline_path}`; a case fails when it is more than {options.threshold:.0%} "
        f"and {options.min_delta_ms:.0f} ms slower than the baseline.",
        "",
        "| case | baseline | now | change | verdict |",
        "| --- | ---: | ---: | ---: | --- |",
    ]
    for verdict in verdicts:
        lines.append(
            f"| `{verdict.case}` | {format_metric(verdict.baseline, verdict.unit)} | "
            f"{format_metric(verdict.current, verdict.unit)} | {verdict.change:+.1%} | "
            f"{'**regressed**' if verdict.regressed else 'ok'} |"
        )
    if warning:
        lines += ["", f"Not enforced: {warning}. Re-record the baseline."]
    return "\n".join(lines) + "\n"


def gate_verdict(verdicts: list[Verdict], warning: str | None) -> int:
    failed = [verdict.case for verdict in verdicts if verdict.regressed]
    if warning:
        print(f"perf gate not enforced: {warning}")
        return 0
    if failed:
        print("perf gate failed: " + ", ".join(failed))
        return 1
    print("perf gate passed")
    return 0


def run_gate(options, table, versions: dict[str, str], retry) -> int:
    baseline = json.loads(Path(options.gate).read_text(encoding="utf-8"))
    subject = baseline["subject"]["name"]
    yardstick = options.yardstick
    verdicts = compare_with_baseline(table, baseline, subject, yardstick, options)

    suspects = {verdict.case for verdict in verdicts if verdict.regressed}
    if suspects and retry is not None:
        print(f"re-measuring {', '.join(sorted(suspects))} before judging", file=sys.stderr)
        table = merge_best(table, retry(suspects), subject, yardstick)
        verdicts = compare_with_baseline(table, baseline, subject, yardstick, options)

    warning = yardstick_mismatch(baseline, versions)
    report = gate_report(verdicts, options.gate, warning, options)
    print(report)
    if options.summary_md:
        with open(options.summary_md, "a", encoding="utf-8") as summary:
            summary.write(report)
    return gate_verdict(verdicts, warning)


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Time shell workloads under several shells.")
    parser.add_argument("--shell", type=parse_shell, action="append", default=[], metavar="NAME=PATH",
                        help="a shell to time; the first one is the baseline")
    parser.add_argument("--runs", type=int, default=10, help="timed runs per case and shell")
    parser.add_argument("--warmup", type=int, default=1, help="untimed runs per case and shell first")
    parser.add_argument("--scale", type=float, default=1.0, help="multiply every loop count")
    parser.add_argument("--filter", action="append", default=[], help="glob on case names, repeatable")
    parser.add_argument("--json", help="write raw samples and statistics as JSON")
    parser.add_argument("--markdown", help="write the markdown report")
    parser.add_argument("--low-priority", action="store_true", help="run below normal priority")
    parser.add_argument("--list", action="store_true", help="list the cases and exit")
    parser.add_argument("--yardstick", metavar="NAME", help="shell whose time every case is divided by")
    parser.add_argument("--write-baseline", metavar="FILE", help="store this run as a perf-gate baseline")
    parser.add_argument("--gate", metavar="FILE", help="fail when a case regressed against this baseline")
    parser.add_argument("--from-json", metavar="FILE", help="judge an earlier --json run instead of measuring")
    parser.add_argument("--threshold", type=float, default=DEFAULT_THRESHOLD,
                        help="relative slowdown that fails the gate (default 0.20)")
    parser.add_argument("--min-delta-ms", type=float, default=DEFAULT_MIN_DELTA_MS,
                        help="absolute slowdown below which the gate never fails (default 3 ms)")
    parser.add_argument("--summary-md", help="append the gate table (e.g. $GITHUB_STEP_SUMMARY)")
    options = parser.parse_args(argv)
    if not options.shell and not options.from_json and not options.list:
        parser.error("--shell is required unless --from-json or --list is given")
    if options.from_json and not options.gate:
        parser.error("--from-json needs --gate")
    return options


def selected_cases(options) -> list[Bench]:
    cases = build_cases(options.scale)
    if not options.filter:
        return cases
    return [case for case in cases if any(fnmatch.fnmatch(case.name, pattern) for pattern in options.filter)]


def write_outputs(options, shells, cases, results) -> None:
    machine = machine_description()
    report = markdown_report(shells, cases, results, machine, options)
    print(report)
    if options.markdown:
        Path(options.markdown).write_text(report, encoding="utf-8")
    if options.json:
        document = json_report(shells, cases, results, machine, options)
        Path(options.json).write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")


def write_baseline(options, table, shells: list[Shell]) -> None:
    yardstick = next((shell for shell in shells if shell.name == options.yardstick), None)
    document = baseline_document(table, shells[0], yardstick, options)
    Path(options.write_baseline).write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    print(f"baseline written to {options.write_baseline}")


def gate_from_json(options) -> int:
    document = json.loads(Path(options.from_json).read_text(encoding="utf-8"))
    versions = {shell["name"]: shell["version"] for shell in document["shells"]}
    return run_gate(options, p50_table_from_json(document), versions, None)


def measure_all(shells, cases, options) -> dict:
    with tempfile.TemporaryDirectory(prefix="wbsh-bench-") as directory:
        workdir = Path(directory)
        prepare_workdir(workdir, cases)
        warm_up(shells, cases, workdir, options)
        return measure(shells, cases, workdir, options)


def main(argv: list[str]) -> int:
    options = parse_arguments(argv)
    cases = selected_cases(options)
    if options.list:
        for case in cases:
            print(f"{case.name:<18} {case.description}")
        return 0
    if options.from_json:
        return gate_from_json(options)

    if options.low_priority:
        lower_own_priority()
    shells = options.shell
    for shell in shells:
        shell.version = shell_version(shell.path)

    results = measure_all(shells, cases, options)
    write_outputs(options, shells, cases, results)

    table = p50_table(results, shells, cases)
    if options.write_baseline:
        write_baseline(options, table, shells)
    if not options.gate:
        return 0
    versions = {shell.name: shell.version for shell in shells}
    return run_gate(options, table, versions, lambda names: remeasure(shells, cases, names, options))


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
