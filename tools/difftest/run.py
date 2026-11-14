#!/usr/bin/env python3
"""
Differential test runner: runs every case under tools/difftest/cases with a
given shell and either records its normalized output as the reference or
compares it against a recorded reference.

Usage:
    python tools/difftest/run.py --shell bash --record ref/
    python tools/difftest/run.py --shell x64/Release/wbsh.exe --compare ref/
    python tools/difftest/run.py --shell wbsh.exe --compare ref/ \\
        --json results.json --summary-md summary.md --baseline tools/difftest/baseline.txt

Each case runs as `<shell> case.sh` in a fresh, empty working directory with
stdin closed. See README.md for the normalizations applied to its output.

Exit status: 0 when the run finished (mismatches are reported, not fatal),
1 when the pass count is below --baseline, 2 on bad usage.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import contextlib
import difflib
import fnmatch
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path


HERE = Path(__file__).resolve().parent
DEFAULT_CASES = HERE / "cases"
SCRIPT_NAME = "case.sh"
REFERENCE_META = "meta.json"
DEFAULT_TIMEOUT = 10.0

REQUIRES_PATTERN = re.compile(r"^#\s*requires:\s*bash\s*>=\s*([0-9.]+)\s*$", re.MULTILINE)

SCRUBBED_VARIABLES = (
    "BASH_ENV", "ENV", "CDPATH", "GLOBIGNORE", "POSIXLY_CORRECT", "SHELLOPTS",
    "BASHOPTS", "IFS", "PS4", "TIMEFORMAT", "HISTFILE", "PROMPT_COMMAND",
    "BASH_COMPAT", "TMOUT", "OLDPWD", "BASH_XTRACEFD", "INPUTRC", "MAILPATH",
    "GLOBSORT", "EXECIGNORE", "FIGNORE", "HOSTFILE",
)

FIXED_VARIABLES = {
    "LC_ALL": "C",
    "LANG": "C",
    "TZ": "UTC",
    "TERM": "dumb",
}

LINE_NUMBER_PREFIX = re.compile(r"^(SHELL: (?:[\w.-]+: )?)line \d+: ", re.MULTILINE)

WINDOWS_BELOW_NORMAL = 0x00004000

CATEGORY_ORDER = ("pass", "stderr", "fail", "timeout", "no-ref", "ref-too-old")


@dataclass
class Case:
    area: str
    name: str
    path: Path
    requires: tuple[int, ...] | None

    @property
    def id(self) -> str:
        return f"{self.area}/{self.name}"


@dataclass
class Outcome:
    status: str
    stdout: str
    stderr: str

    def render(self) -> str:
        return f"--- status {self.status}\n--- stdout\n{self.stdout}--- stderr\n{self.stderr}"


@dataclass
class Result:
    case: Case
    category: str
    actual: Outcome
    expected: Outcome | None = None
    seconds: float = 0.0


@dataclass
class Normalizer:
    shell_names: list[str]
    work_paths: list[str] = field(default_factory=list)

    def apply(self, text: str) -> str:
        text = text.replace("\r\n", "\n")
        text = self.replace_work_paths(text)
        return self.replace_name_prefixes(text)

    def replace_work_paths(self, text: str) -> str:
        for path in sorted(self.work_paths, key=len, reverse=True):
            text = re.sub(re.escape(path), "$WORK", text, flags=re.IGNORECASE)
        return text

    def replace_name_prefixes(self, text: str) -> str:
        names = "|".join(re.escape(name) for name in self.shell_names)
        prefix = re.compile(rf"^(?:(?:{names}): )+", re.MULTILINE)
        return prefix.sub("SHELL: ", text)


def parse_version(text: str) -> tuple[int, ...]:
    numbers = re.findall(r"\d+", text)
    return tuple(int(number) for number in numbers[:3])


def format_version(version: tuple[int, ...] | None) -> str:
    return ".".join(str(part) for part in version) if version else "?"


def read_requires(path: Path) -> tuple[int, ...] | None:
    head = "\n".join(path.read_text(encoding="utf-8").splitlines()[:5])
    match = REQUIRES_PATTERN.search(head)
    return parse_version(match.group(1)) if match else None


def discover_cases(root: Path, patterns: list[str]) -> list[Case]:
    cases = []
    for path in sorted(root.glob("*/*.sh")):
        case = Case(path.parent.name, path.stem, path, read_requires(path))
        if not patterns or any(fnmatch.fnmatch(case.id, pattern) for pattern in patterns):
            cases.append(case)
    return cases


def path_variants(path: Path) -> list[str]:
    native = str(path)
    forward = native.replace("\\", "/")
    variants = {native, forward}
    if re.match(r"^[A-Za-z]:/", forward):
        variants.add(f"/{forward[0].lower()}{forward[2:]}")
        variants.add(f"/cygdrive/{forward[0].lower()}{forward[2:]}")
    return sorted(variants, key=len, reverse=True)


def work_path_variants(workdir: Path) -> list[str]:
    variants = path_variants(workdir)
    resolved = workdir.resolve()
    if resolved != workdir:
        variants += path_variants(resolved)
    return variants


def shell_name_aliases(shell_command: list[str]) -> list[str]:
    stem = Path(shell_command[0]).stem
    names = {stem, f"{stem}.exe", "bash", "wbsh", "sh", SCRIPT_NAME, f"./{SCRIPT_NAME}"}
    return sorted(names, key=len, reverse=True)


def build_environment(workdir: Path) -> dict[str, str]:
    env = {
        key: value for key, value in os.environ.items()
        if key.upper() not in SCRUBBED_VARIABLES
        and not key.startswith("BASH_FUNC_")
        and not key.startswith("LC_")
    }
    env.update(FIXED_VARIABLES)
    env["HOME"] = str(workdir)
    return env


def creation_flags(low_priority: bool) -> int:
    if os.name == "nt" and low_priority:
        return WINDOWS_BELOW_NORMAL
    return 0


def lower_own_priority() -> None:
    if os.name == "nt":
        import ctypes
        kernel32 = ctypes.windll.kernel32
        kernel32.SetPriorityClass(kernel32.GetCurrentProcess(), WINDOWS_BELOW_NORMAL)
    else:
        os.nice(10)


def kill_tree(process: subprocess.Popen) -> None:
    if os.name == "nt":
        subprocess.run(
            ["taskkill", "/F", "/T", "/PID", str(process.pid)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
    else:
        with contextlib.suppress(ProcessLookupError):
            os.killpg(process.pid, signal.SIGKILL)
    process.kill()


def decode(data: bytes) -> str:
    return data.decode("utf-8", errors="backslashreplace")


def execute(command: list[str], workdir: Path, timeout: float, low_priority: bool) -> tuple[str, bytes, bytes]:
    process = subprocess.Popen(
        command, cwd=workdir, env=build_environment(workdir),
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        creationflags=creation_flags(low_priority), start_new_session=os.name != "nt",
    )
    try:
        stdout, stderr = process.communicate(timeout=timeout)
        return str(process.returncode), stdout, stderr
    except subprocess.TimeoutExpired:
        kill_tree(process)
        stdout, stderr = process.communicate()
        return "timeout", stdout, stderr


def prepare_workdir(scratch: Path, case: Case) -> Path:
    workdir = scratch / case.area / case.name
    if workdir.exists():
        shutil.rmtree(workdir, ignore_errors=True)
    workdir.mkdir(parents=True)
    shutil.copyfile(case.path, workdir / SCRIPT_NAME)
    return workdir


def run_case(case: Case, shell_command: list[str], scratch: Path, options: argparse.Namespace) -> Outcome:
    workdir = prepare_workdir(scratch, case)
    status, stdout, stderr = execute(
        [*shell_command, SCRIPT_NAME], workdir, options.timeout, options.low_priority)
    normalizer = Normalizer(shell_name_aliases(shell_command), work_path_variants(workdir))
    if not options.keep:
        shutil.rmtree(workdir, ignore_errors=True)
    return Outcome(status, normalizer.apply(decode(stdout)), normalizer.apply(decode(stderr)))


def query_version(shell_command: list[str]) -> tuple[int, ...] | None:
    try:
        completed = subprocess.run(
            [*shell_command, "-c", 'printf %s "$BASH_VERSION"'],
            stdin=subprocess.DEVNULL, capture_output=True, timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    version = parse_version(decode(completed.stdout))
    return version or None


def parse_rendered(text: str) -> Outcome | None:
    match = re.match(r"--- status (\S+)\n--- stdout\n(.*)--- stderr\n(.*)\Z", text, re.DOTALL)
    if not match:
        return None
    return Outcome(match.group(1), match.group(2), match.group(3))


def reference_file(reference: Path, case: Case) -> Path:
    return reference / case.area / f"{case.name}.out"


def load_expected(reference: Path, case: Case) -> Outcome | None:
    path = reference_file(reference, case)
    if not path.is_file():
        return None
    return parse_rendered(path.read_bytes().decode("utf-8", errors="backslashreplace"))


def save_reference(reference: Path, case: Case, outcome: Outcome) -> None:
    path = reference_file(reference, case)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(outcome.render().encode("utf-8", errors="backslashreplace"))


def load_reference_meta(reference: Path) -> dict:
    path = reference / REFERENCE_META
    if not path.is_file():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))


def save_reference_meta(reference: Path, shell_command: list[str], version, cases: list[Case]) -> None:
    meta = {
        "shell": shell_command,
        "version": list(version) if version else None,
        "cases": len(cases),
        "recorded": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }
    (reference / REFERENCE_META).write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")


def drop_line_numbers(outcome: Outcome) -> Outcome:
    def strip(text: str) -> str:
        return LINE_NUMBER_PREFIX.sub(r"\1", text)

    return Outcome(outcome.status, strip(outcome.stdout), strip(outcome.stderr))


def needs_newer(case: Case, version: tuple[int, ...] | None) -> bool:
    return bool(case.requires and version and version < case.requires)


def classify(case: Case, actual: Outcome, expected: Outcome | None, reference_version) -> str:
    if needs_newer(case, reference_version):
        return "ref-too-old"
    if expected is None:
        return "no-ref"
    if actual.status == "timeout":
        return "timeout"
    if actual.render() == expected.render():
        return "pass"
    if actual.status == expected.status and actual.stdout == expected.stdout:
        return "stderr"
    return "fail"


def run_all(cases: list[Case], shell_command: list[str], options: argparse.Namespace, judge) -> list[Result]:
    scratch = Path(options.work_root or tempfile.mkdtemp(prefix="difftest-"))
    scratch.mkdir(parents=True, exist_ok=True)

    def one(case: Case) -> Result:
        started = time.perf_counter()
        outcome = run_case(case, shell_command, scratch, options)
        result = judge(case, outcome)
        result.seconds = time.perf_counter() - started
        return result

    with concurrent.futures.ThreadPoolExecutor(max_workers=options.jobs) as pool:
        results = list(pool.map(one, cases))

    if not options.keep and not options.work_root:
        shutil.rmtree(scratch, ignore_errors=True)
    return results


def count_categories(results: list[Result]) -> dict[str, int]:
    counts = {category: 0 for category in CATEGORY_ORDER}
    for result in results:
        counts[result.category] = counts.get(result.category, 0) + 1
    return counts


def status_column(result: Result) -> str:
    if result.expected is None or result.expected.status == result.actual.status:
        return f"exit {result.actual.status}"
    return f"exit {result.actual.status} (want {result.expected.status})"


def print_table(results: list[Result], quiet: bool) -> None:
    width = max((len(result.case.id) for result in results), default=10)
    for result in results:
        if quiet and result.category == "pass":
            continue
        print(f"{result.category:<12} {result.case.id:<{width}}  {status_column(result)}")


def unified_diff(result: Result) -> str:
    expected = result.expected.render().splitlines(keepends=True)
    actual = result.actual.render().splitlines(keepends=True)
    lines = difflib.unified_diff(expected, actual, "reference", "actual")
    return "".join(line if line.endswith("\n") else line + "\n" for line in lines)


def print_diffs(results: list[Result]) -> None:
    for result in results:
        if result.category in ("fail", "stderr", "timeout") and result.expected:
            print(f"\n=== {result.case.id} ({result.category})")
            sys.stdout.write(unified_diff(result))


def comparable_total(counts: dict[str, int]) -> int:
    return counts["pass"] + counts["stderr"] + counts["fail"] + counts["timeout"]


def summary_line(counts: dict[str, int]) -> str:
    total = comparable_total(counts)
    percent = 100.0 * counts["pass"] / total if total else 0.0
    parts = [f"{category} {counts[category]}" for category in CATEGORY_ORDER if counts[category]]
    return f"{counts['pass']}/{total} pass ({percent:.1f}%) -- " + ", ".join(parts)


def results_document(results, shell_command, version, reference_meta, counts) -> dict:
    return {
        "shell": shell_command,
        "shell_version": format_version(version),
        "reference": reference_meta,
        "summary": counts,
        "cases": [
            {
                "id": result.case.id,
                "category": result.category,
                "requires": format_version(result.case.requires) if result.case.requires else None,
                "status": result.actual.status,
                "expected_status": result.expected.status if result.expected else None,
                "seconds": round(result.seconds, 3),
            }
            for result in results
        ],
    }


def markdown_summary(results, counts, version, reference_meta, baseline) -> str:
    reference_version = format_version(tuple(reference_meta.get("version") or ()))
    lines = [
        "## Differential tests",
        "",
        f"Shell under test: bash-version `{format_version(version)}`; "
        f"reference: bash `{reference_version}`.",
        "",
        f"**{summary_line(counts)}**",
        "",
    ]
    if baseline is not None:
        lines += [f"Baseline: {baseline} passing cases.", ""]
    lines += ["| category | cases |", "| --- | ---: |"]
    lines += [f"| {category} | {counts[category]} |" for category in CATEGORY_ORDER]
    lines += mismatch_details(results)
    return "\n".join(lines) + "\n"


def mismatch_details(results: list[Result]) -> list[str]:
    mismatched = [result for result in results if result.category in ("fail", "stderr", "timeout")]
    if not mismatched:
        return []
    lines = ["", "<details><summary>Mismatched cases</summary>", ""]
    lines += [f"- `{result.case.id}`: {result.category}, {status_column(result)}" for result in mismatched]
    lines += ["", "</details>"]
    return lines


def read_baseline(path: str | None) -> int | None:
    if not path:
        return None
    return int(Path(path).read_text(encoding="utf-8").split()[0])


def record_mode(cases, shell_command, options) -> int:
    reference = Path(options.record)
    version = query_version(shell_command)

    def judge(case: Case, outcome: Outcome) -> Result:
        save_reference(reference, case, outcome)
        category = "ref-too-old" if needs_newer(case, version) else "pass"
        return Result(case, category, outcome)

    results = run_all(cases, shell_command, options, judge)
    save_reference_meta(reference, shell_command, version, cases)

    newer = sum(1 for result in results if result.category == "ref-too-old")
    timeouts = sum(1 for result in results if result.actual.status == "timeout")
    print(f"recorded {len(results)} cases with bash {format_version(version)} into {reference}")
    print(f"{newer} cases need a newer bash than this one; {timeouts} timed out")
    return 0


def compare_mode(cases, shell_command, options) -> int:
    reference = Path(options.compare)
    reference_meta = load_reference_meta(reference)
    reference_version = tuple(reference_meta.get("version") or ()) or None

    def judge(case: Case, outcome: Outcome) -> Result:
        expected = load_expected(reference, case)
        if options.ignore_line_numbers:
            outcome = drop_line_numbers(outcome)
            expected = drop_line_numbers(expected) if expected else None
        return Result(case, classify(case, outcome, expected, reference_version), outcome, expected)

    results = run_all(cases, shell_command, options, judge)
    counts = count_categories(results)
    version = query_version(shell_command)
    baseline = read_baseline(options.baseline)

    print_table(results, options.quiet)
    if options.diff:
        print_diffs(results)
    print()
    print(summary_line(counts))

    write_reports(options, results, shell_command, version, reference_meta, counts, baseline)
    return baseline_verdict(counts, baseline)


def write_reports(options, results, shell_command, version, reference_meta, counts, baseline) -> None:
    if options.json:
        document = results_document(results, shell_command, version, reference_meta, counts)
        Path(options.json).write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    if options.summary_md:
        with open(options.summary_md, "a", encoding="utf-8") as summary:
            summary.write(markdown_summary(results, counts, version, reference_meta, baseline))
    if options.diff_dir:
        write_diff_files(Path(options.diff_dir), results)


def write_diff_files(directory: Path, results: list[Result]) -> None:
    for result in results:
        if result.category in ("fail", "stderr", "timeout") and result.expected:
            path = directory / result.case.area / f"{result.case.name}.diff"
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(unified_diff(result).encode("utf-8", errors="backslashreplace"))


def baseline_verdict(counts: dict[str, int], baseline: int | None) -> int:
    if baseline is None or counts["pass"] >= baseline:
        return 0
    print(f"pass count {counts['pass']} is below the baseline of {baseline}")
    return 1


def list_mode(cases: list[Case]) -> int:
    for case in cases:
        requires = f"  (bash>={format_version(case.requires)})" if case.requires else ""
        print(f"{case.id}{requires}")
    print(f"{len(cases)} cases")
    return 0


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run the differential corpus under a shell.")
    parser.add_argument("--shell", help="shell executable to run every case with")
    parser.add_argument("--shell-arg", action="append", default=[], help="extra argument before case.sh")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--record", metavar="DIR", help="write reference outputs into DIR")
    mode.add_argument("--compare", metavar="DIR", help="compare against reference outputs in DIR")
    mode.add_argument("--list", action="store_true", help="list the cases and exit")
    parser.add_argument("--cases", default=str(DEFAULT_CASES), help="corpus root (default: cases/)")
    parser.add_argument("--filter", action="append", default=[], help="glob on area/name, repeatable")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT, help="seconds per case")
    parser.add_argument("--jobs", type=int, default=4, help="cases run in parallel")
    parser.add_argument("--json", help="write per-case results as JSON")
    parser.add_argument("--summary-md", help="append a markdown summary (e.g. $GITHUB_STEP_SUMMARY)")
    parser.add_argument("--diff-dir", help="write a .diff per mismatched case into this directory")
    parser.add_argument("--baseline", help="file whose first number is the minimum pass count")
    parser.add_argument("--diff", action="store_true", help="print unified diffs of mismatches")
    parser.add_argument("--quiet", action="store_true", help="leave passing cases out of the table")
    parser.add_argument("--keep", action="store_true", help="keep the per-case working directories")
    parser.add_argument("--work-root", help="directory for per-case working directories")
    parser.add_argument(
        "--ignore-line-numbers", action="store_true",
        help="drop the 'line N: ' part of error messages on both sides (compare mode only)")
    parser.add_argument("--low-priority", action="store_true", help="run below normal priority")
    options = parser.parse_args(argv)
    if not options.list and not options.shell:
        parser.error("--shell is required with --record and --compare")
    return options


def resolve_shell(shell: str) -> str | None:
    if Path(shell).is_file():
        return str(Path(shell).resolve())
    return shutil.which(shell)


def main(argv: list[str]) -> int:
    options = parse_arguments(argv)
    cases = discover_cases(Path(options.cases), options.filter)
    if options.list:
        return list_mode(cases)
    if not cases:
        print("no cases matched", file=sys.stderr)
        return 2
    if options.low_priority:
        lower_own_priority()

    shell = resolve_shell(options.shell)
    if shell is None:
        print(f"shell not found: {options.shell}", file=sys.stderr)
        return 2

    shell_command = [shell, *options.shell_arg]
    if options.record:
        return record_mode(cases, shell_command, options)
    return compare_mode(cases, shell_command, options)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
