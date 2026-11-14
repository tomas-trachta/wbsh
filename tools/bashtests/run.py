#!/usr/bin/env python3
"""
GNU bash upstream test-suite runner: runs every `run-*` script from a
bash-5.3 source tree's tests/ directory under a given shell, compares the
output with the suite's `.right` file and classifies each suite as
identical, env-only or fail.

Usage:
    python -I tools/bashtests/run.py --shell x64/Release/wbsh.exe --fetch C:/cache/bash-src
    python -I tools/bashtests/run.py --shell wbsh.exe --tests C:/src/bash-5.3/tests \\
        --json results.json --markdown ledger-table.md --diff-dir diffs

The shell is copied into a private bin directory as `bash.exe` (so error
messages and `$THIS_SH` look like they do under `make tests`), next to
script versions of bash's recho / zecho / xcase helpers. Each suite runs in
its own fresh copy of tests/, with HOME and TMPDIR inside it and stdin
closed. See README.md for the classification rules.

Exit status: 0 when the run finished (failures are reported, not fatal),
1 when fewer suites are identical than --baseline says, 2 on bad usage.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import contextlib
import difflib
import fnmatch
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path


HERE = Path(__file__).resolve().parent
DEFAULT_RULES = HERE / "env_rules.json"

BASH_VERSION = "5.3"
BASH_URL = f"https://ftp.gnu.org/gnu/bash/bash-{BASH_VERSION}.tar.gz"
BASH_SHA256 = "0d5cd86965f869a26cf64f4b71be7b96f90a3ba8b3d74e27e8e9d9d5550f31ba"
TESTS_MEMBER_PREFIX = f"bash-{BASH_VERSION}/tests/"

DEFAULT_TIMEOUT = 180.0
SHELL_NAME = "bash"
OUTPUT_NAME = "bashtst-out"
SKIPPED_SCRIPTS = ("run-all", "run-minimal", "run-gprof")

SCRUBBED_VARIABLES = (
    "BASH_ENV", "ENV", "CDPATH", "GLOBIGNORE", "POSIXLY_CORRECT", "SHELLOPTS",
    "BASHOPTS", "IFS", "PS4", "TIMEFORMAT", "HISTFILE", "HISTFILESIZE", "HISTSIZE",
    "PROMPT_COMMAND", "BASH_COMPAT", "TMOUT", "OLDPWD", "BASH_XTRACEFD", "INPUTRC",
    "MAILPATH", "MAIL", "GLOBSORT", "EXECIGNORE", "FIGNORE", "HOSTFILE", "LANGUAGE",
    "MSYSTEM", "TMP", "TEMP",
)

FIXED_VARIABLES = {
    "LANG": "C",
    "TZ": "UTC",
    "TERM": "dumb",
}

WINDOWS_BELOW_NORMAL = 0x00004000

CATEGORY_ORDER = ("identical", "env-only", "fail", "timeout", "error")

RECHO_SCRIPT = r"""#!/bin/sh
n=0
for arg
do
    n=$((n + 1))
    printf 'argv[%d] = <' "$n"
    case $arg in
        *[[:cntrl:]]*)
            while [ -n "$arg" ]; do
                rest=${arg#?}
                c=${arg%"$rest"}
                arg=$rest
                case $c in
                    [[:cntrl:]])
                        code=$(printf '%d' "'$c")
                        if [ "$code" -eq 127 ]; then
                            printf '^?'
                        else
                            printf "^\\$(printf '%03o' $((code + 64)))"
                        fi
                        ;;
                    *)
                        printf '%s' "$c"
                        ;;
                esac
            done
            ;;
        *)
            printf '%s' "$arg"
            ;;
    esac
    printf '>\n'
done
"""

ZECHO_SCRIPT = r"""#!/bin/sh
IFS=' '
printf '%s\n' "$*"
"""

XCASE_SCRIPT = r"""#!/bin/sh
op=
while getopts lnu flag; do
    case $flag in
        u) op=upper ;;
        l) op=lower ;;
        n) ;;
        *) echo "casemod: usage: casemod [-lnu] [file]" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
case $op in
    upper) set -- '[:lower:]' '[:upper:]' "$@" ;;
    lower) set -- '[:upper:]' '[:lower:]' "$@" ;;
    *) set -- '' '' "$@" ;;
esac
if [ -n "$3" ] && [ "$3" != - ]; then
    exec < "$3"
fi
if [ -n "$1" ]; then
    exec tr "$1" "$2"
fi
exec cat
"""

HELPER_SCRIPTS = {"recho": RECHO_SCRIPT, "zecho": ZECHO_SCRIPT, "xcase": XCASE_SCRIPT}

RIGHT_FILE_PATTERN = re.compile(r"^\s*diff\b.*?(\S+\.right)\b", re.MULTILINE)
CLEANUP_PATTERN = re.compile(r"\s*&&\s*rm -f \S+\s*$", re.MULTILINE)
TEST_NAME_PATTERN = re.compile(r"^TEST_NAME='([\w.-]+)'", re.MULTILINE)
FIXED_CHECK_FILE = re.compile(r'TEST_FILE="/tmp/\$\{TEST_NAME\}\.check"')


@dataclass
class Suite:
    name: str
    script: Path
    right: str

    @property
    def id(self) -> str:
        return self.name[len("run-"):]


@dataclass
class EnvRule:
    id: str
    reason: str
    pattern: re.Pattern | None = None
    suites: tuple[str, ...] = ()


@dataclass
class Result:
    suite: Suite
    category: str
    seconds: float = 0.0
    exit_status: str = ""
    diff: str = ""
    changed_lines: int = 0
    rules: list[str] = field(default_factory=list)
    note: str = ""


@dataclass
class Sandbox:
    bin_dir: Path
    shell: Path
    tests: Path


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def download_tarball(cache: Path) -> Path:
    tarball = cache / f"bash-{BASH_VERSION}.tar.gz"
    if tarball.is_file() and sha256_of(tarball) == BASH_SHA256:
        return tarball

    partial = tarball.with_suffix(".part")
    print(f"downloading {BASH_URL}", file=sys.stderr)
    with urllib.request.urlopen(BASH_URL, timeout=120) as response, partial.open("wb") as stream:
        shutil.copyfileobj(response, stream)

    actual = sha256_of(partial)
    if actual != BASH_SHA256:
        partial.unlink()
        raise SystemExit(f"sha256 mismatch for {BASH_URL}: got {actual}")
    partial.replace(tarball)
    return tarball


def safe_tests_members(archive: tarfile.TarFile) -> list[tarfile.TarInfo]:
    members = []
    for member in archive.getmembers():
        if not member.name.startswith(TESTS_MEMBER_PREFIX):
            continue
        if ".." in Path(member.name).parts or not (member.isfile() or member.isdir()):
            continue
        members.append(member)
    return members


def extract_tests(tarball: Path, cache: Path) -> Path:
    tests = cache / TESTS_MEMBER_PREFIX
    if (tests / "run-all").is_file():
        return tests

    extract_options = {"filter": "data"} if hasattr(tarfile, "data_filter") else {}
    with tarfile.open(tarball, "r:gz") as archive:
        for member in safe_tests_members(archive):
            archive.extract(member, cache, **extract_options)
    return tests


def fetch_tests(cache: Path) -> Path:
    cache.mkdir(parents=True, exist_ok=True)
    tarball = download_tarball(cache)
    return extract_tests(tarball, cache)


def right_file_of(script_text: str) -> str | None:
    live_lines = "\n".join(line for line in script_text.splitlines() if not line.lstrip().startswith("#"))
    matches = RIGHT_FILE_PATTERN.findall(live_lines)
    if not matches:
        return None
    test_name = TEST_NAME_PATTERN.search(live_lines)
    if test_name:
        return matches[-1].replace("${TEST_NAME}", test_name.group(1))
    return matches[-1]


def is_suite_script(path: Path) -> bool:
    name = path.name
    if name in SKIPPED_SCRIPTS or name.endswith((".orig", "~")):
        return False
    return path.is_file() and name.startswith("run-")


def discover_suites(tests: Path, patterns: list[str]) -> list[Suite]:
    suites = []
    for path in sorted(tests.iterdir()):
        if not is_suite_script(path):
            continue
        right = right_file_of(path.read_text(encoding="utf-8", errors="replace"))
        suite = Suite(path.name, path, right or "")
        if not patterns or any(fnmatch.fnmatch(suite.id, pattern) for pattern in patterns):
            suites.append(suite)
    return suites


def load_rules(path: Path) -> list[EnvRule]:
    document = json.loads(path.read_text(encoding="utf-8"))
    rules = []
    for entry in document["line_rules"]:
        rules.append(EnvRule(entry["id"], entry["reason"], re.compile(entry["pattern"])))
    for entry in document["suite_rules"]:
        rules.append(EnvRule(entry["id"], entry["reason"], suites=tuple(entry["suites"])))
    return rules


def install_helpers(bin_dir: Path) -> None:
    for name, text in HELPER_SCRIPTS.items():
        (bin_dir / name).write_bytes(text.encode("ascii"))


def prepare_sandbox(shell: str, tests: Path, root: Path) -> Sandbox:
    bin_dir = root / "bin"
    bin_dir.mkdir(parents=True, exist_ok=True)
    copied = bin_dir / f"{SHELL_NAME}.exe" if os.name == "nt" else bin_dir / SHELL_NAME
    shutil.copy2(shell, copied)
    install_helpers(bin_dir)
    return Sandbox(bin_dir, copied, tests)


def patch_run_script(text: str) -> str:
    text = FIXED_CHECK_FILE.sub('TEST_FILE="${BASH_TSTOUT}"', text)
    return CLEANUP_PATTERN.sub("", text)


def prepare_suite_dir(sandbox: Sandbox, suite: Suite, scratch: Path) -> Path:
    workdir = scratch / suite.id
    if workdir.exists():
        shutil.rmtree(workdir, ignore_errors=True)
    shutil.copytree(sandbox.tests, workdir / "tests")
    (workdir / "tmp").mkdir()
    (workdir / "home").mkdir()

    script = workdir / "tests" / suite.name
    script.write_bytes(patch_run_script(suite.script.read_text(encoding="utf-8")).encode("utf-8"))
    return workdir


def shell_path_form(path: Path) -> str:
    return str(path).replace("\\", "/")


def system_path_entries() -> list[str]:
    if os.name != "nt":
        return ["/usr/local/bin", "/usr/bin", "/bin"]
    system_root = os.environ.get("SystemRoot", r"C:\Windows")
    return [str(Path(system_root) / "System32"), system_root]


def is_scrubbed(key: str) -> bool:
    upper = key.upper()
    return (
        upper in SCRUBBED_VARIABLES
        or upper.startswith(("LC_", "WBSH_", "BASH_FUNC_"))
        or upper == "PATH"
    )


def build_environment(sandbox: Sandbox, workdir: Path) -> dict[str, str]:
    env = {key: value for key, value in os.environ.items() if not is_scrubbed(key)}
    env.update(FIXED_VARIABLES)

    this_sh = sandbox.shell.with_suffix("") if os.name == "nt" else sandbox.shell
    env["THIS_SH"] = shell_path_form(this_sh)
    env["BUILD_DIR"] = shell_path_form(workdir)
    env["TMPDIR"] = shell_path_form(workdir / "tmp")
    env["BASH_TSTOUT"] = shell_path_form(workdir / "tmp" / OUTPUT_NAME)
    env["HOME"] = shell_path_form(workdir / "home")
    env["PATH"] = os.pathsep.join([str(sandbox.bin_dir), ".", *system_path_entries()])
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


def execute_suite(sandbox: Sandbox, suite: Suite, workdir: Path, options) -> str:
    process = subprocess.Popen(
        [str(sandbox.shell), suite.name], cwd=workdir / "tests",
        env=build_environment(sandbox, workdir),
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        creationflags=creation_flags(options.low_priority), start_new_session=os.name != "nt",
    )
    try:
        process.wait(timeout=options.timeout)
        return str(process.returncode)
    except subprocess.TimeoutExpired:
        kill_tree(process)
        process.wait()
        return "timeout"


def read_lines(path: Path) -> list[str]:
    if not path.is_file():
        return []
    return path.read_bytes().decode("utf-8", errors="backslashreplace").splitlines(keepends=True)


def changed_lines(expected: list[str], actual: list[str]) -> tuple[list[str], list[str]]:
    removed, added = [], []
    matcher = difflib.SequenceMatcher(None, expected, actual, autojunk=False)
    for tag, low_a, high_a, low_b, high_b in matcher.get_opcodes():
        if tag != "equal":
            removed += expected[low_a:high_a]
            added += actual[low_b:high_b]
    return removed, added


def matching_line_rules(lines: list[str], rules: list[EnvRule]) -> list[str] | None:
    used = set()
    for line in lines:
        rule = next((rule for rule in rules if rule.pattern and rule.pattern.search(line)), None)
        if rule is None:
            return None
        used.add(rule.id)
    return sorted(used)


def suite_rule_for(suite: Suite, rules: list[EnvRule]) -> EnvRule | None:
    return next((rule for rule in rules if suite.id in rule.suites), None)


def unified_diff(expected: list[str], actual: list[str], right: str) -> str:
    lines = difflib.unified_diff(expected, actual, right, "actual")
    return "".join(line if line.endswith("\n") else line + "\n" for line in lines)


def judge(suite: Suite, workdir: Path, status: str, rules: list[EnvRule]) -> Result:
    expected = read_lines(workdir / "tests" / suite.right)
    actual = read_lines(workdir / "tmp" / OUTPUT_NAME)
    result = Result(suite, "fail", exit_status=status)
    if not expected:
        result.category, result.note = "error", f"no {suite.right or '.right file'} found"
        return result
    if expected == actual and status != "timeout":
        result.category = "identical"
        return result

    removed, added = changed_lines(expected, actual)
    result.diff = unified_diff(expected, actual, suite.right)
    result.changed_lines = len(removed) + len(added)
    classify_difference(result, removed + added, status, rules)
    return result


def classify_difference(result: Result, lines: list[str], status: str, rules: list[EnvRule]) -> None:
    if status == "timeout":
        result.category = "timeout"
        return

    used = matching_line_rules(lines, rules)
    if used:
        result.category, result.rules = "env-only", used
        return

    suite_rule = suite_rule_for(result.suite, rules)
    if suite_rule:
        result.category, result.rules = "env-only", [suite_rule.id]


def run_suite(sandbox: Sandbox, suite: Suite, scratch: Path, rules: list[EnvRule], options) -> Result:
    started = time.perf_counter()
    workdir = prepare_suite_dir(sandbox, suite, scratch)
    status = execute_suite(sandbox, suite, workdir, options)
    result = judge(suite, workdir, status, rules)
    result.seconds = time.perf_counter() - started
    if not options.keep:
        shutil.rmtree(workdir, ignore_errors=True)
    print(f"{result.category:<10} {suite.id:<16} {result.seconds:6.1f}s", file=sys.stderr, flush=True)
    return result


def run_all(sandbox: Sandbox, suites: list[Suite], scratch: Path, rules: list[EnvRule], options) -> list[Result]:
    def one(suite: Suite) -> Result:
        return run_suite(sandbox, suite, scratch, rules, options)

    with concurrent.futures.ThreadPoolExecutor(max_workers=options.jobs) as pool:
        return list(pool.map(one, suites))


def count_categories(results: list[Result]) -> dict[str, int]:
    counts = {category: 0 for category in CATEGORY_ORDER}
    for result in results:
        counts[result.category] += 1
    return counts


def summary_line(counts: dict[str, int], total: int) -> str:
    real = counts["fail"] + counts["timeout"] + counts["error"]
    return (
        f"{counts['identical']}/{total} byte-identical, {counts['env-only']} env-only, "
        f"{real} real failures ({counts['timeout']} timeouts)"
    )


def rule_reasons(rules: list[EnvRule]) -> dict[str, str]:
    return {rule.id: rule.reason for rule in rules}


def detail_column(result: Result) -> str:
    if result.category == "identical":
        return ""
    if result.rules:
        return ", ".join(result.rules)
    if result.note:
        return result.note
    return f"{result.changed_lines} lines differ, exit {result.exit_status}"


def print_table(results: list[Result]) -> None:
    for result in results:
        print(f"{result.category:<10} {result.suite.id:<16} {result.seconds:6.1f}s  {detail_column(result)}")


def markdown_table(results: list[Result]) -> str:
    lines = ["| suite | result | changed lines | seconds | detail |", "| --- | --- | ---: | ---: | --- |"]
    for result in results:
        lines.append(
            f"| `{result.suite.id}` | {result.category} | {result.changed_lines} | "
            f"{result.seconds:.1f} | {detail_column(result)} |"
        )
    return "\n".join(lines)


def markdown_summary(results: list[Result], counts: dict[str, int], shell_version: str) -> str:
    lines = [
        f"## GNU bash {BASH_VERSION} upstream suites",
        "",
        f"Shell under test: `{shell_version}`.",
        "",
        f"**{summary_line(counts, len(results))}**",
        "",
        "<details><summary>Per suite</summary>",
        "",
        markdown_table(results),
        "",
        "</details>",
    ]
    return "\n".join(lines) + "\n"


def results_document(results: list[Result], counts: dict[str, int], rules, shell_version: str) -> dict:
    return {
        "bash_version": BASH_VERSION,
        "shell_version": shell_version,
        "recorded": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "summary": {**counts, "total": len(results)},
        "rules": rule_reasons(rules),
        "suites": [
            {
                "id": result.suite.id,
                "category": result.category,
                "exit_status": result.exit_status,
                "changed_lines": result.changed_lines,
                "seconds": round(result.seconds, 2),
                "rules": result.rules,
                "note": result.note,
            }
            for result in results
        ],
    }


def write_diff_files(directory: Path, results: list[Result]) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    for result in results:
        if result.diff:
            (directory / f"{result.suite.id}.diff").write_bytes(
                result.diff.encode("utf-8", errors="backslashreplace"))


def write_reports(options, results, counts, rules, shell_version: str) -> None:
    if options.json:
        document = results_document(results, counts, rules, shell_version)
        Path(options.json).write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    if options.markdown:
        Path(options.markdown).write_text(markdown_table(results) + "\n", encoding="utf-8")
    if options.summary_md:
        with open(options.summary_md, "a", encoding="utf-8") as summary:
            summary.write(markdown_summary(results, counts, shell_version))
    if options.diff_dir:
        write_diff_files(Path(options.diff_dir), results)


def query_version(shell: str) -> str:
    try:
        completed = subprocess.run(
            [shell, "--version"], stdin=subprocess.DEVNULL, capture_output=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return "?"
    first_line = completed.stdout.decode("utf-8", errors="replace").strip().splitlines()[:1]
    return first_line[0] if first_line else "?"


def read_baseline(path: str | None) -> int | None:
    if not path:
        return None
    return int(Path(path).read_text(encoding="utf-8").split()[0])


def baseline_verdict(counts: dict[str, int], baseline: int | None) -> int:
    if baseline is None or counts["identical"] >= baseline:
        return 0
    print(f"{counts['identical']} identical suites is below the baseline of {baseline}")
    return 1


def resolve_shell(shell: str) -> str | None:
    if Path(shell).is_file():
        return str(Path(shell).resolve())
    return shutil.which(shell)


def resolve_tests(options) -> Path:
    if options.tests:
        return Path(options.tests).resolve()
    return fetch_tests(Path(options.fetch).resolve())


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run GNU bash's own test suites under a shell.")
    parser.add_argument("--shell", required=True, help="shell executable to test")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--tests", help="an extracted bash-5.3/tests directory")
    source.add_argument("--fetch", metavar="DIR", help="download, verify and extract bash-5.3 into DIR")
    parser.add_argument("--rules", default=str(DEFAULT_RULES), help="env-only rules (JSON)")
    parser.add_argument("--filter", action="append", default=[], help="glob on suite names, repeatable")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT, help="seconds per suite")
    parser.add_argument("--jobs", type=int, default=1, help="suites run in parallel")
    parser.add_argument("--json", help="write per-suite results as JSON")
    parser.add_argument("--markdown", help="write the per-suite markdown table")
    parser.add_argument("--summary-md", help="append a markdown summary (e.g. $GITHUB_STEP_SUMMARY)")
    parser.add_argument("--diff-dir", help="write <suite>.diff for every suite that is not identical")
    parser.add_argument("--baseline", help="file whose first number is the minimum identical count")
    parser.add_argument("--keep", action="store_true", help="keep the per-suite working directories")
    parser.add_argument("--work-root", help="directory for the sandbox and per-suite directories")
    parser.add_argument("--low-priority", action="store_true", help="run below normal priority")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    options = parse_arguments(argv)
    shell = resolve_shell(options.shell)
    if shell is None:
        print(f"shell not found: {options.shell}", file=sys.stderr)
        return 2
    if options.low_priority:
        lower_own_priority()

    tests = resolve_tests(options)
    suites = discover_suites(tests, options.filter)
    if not suites:
        print(f"no suites found in {tests}", file=sys.stderr)
        return 2
    rules = load_rules(Path(options.rules))

    scratch = Path(options.work_root or tempfile.mkdtemp(prefix="bashtests-"))
    sandbox = prepare_sandbox(shell, tests, scratch)
    results = run_all(sandbox, suites, scratch, rules, options)
    if not options.keep and not options.work_root:
        shutil.rmtree(scratch, ignore_errors=True)

    counts = count_categories(results)
    shell_version = query_version(shell)
    print_table(results)
    print()
    print(summary_line(counts, len(results)))
    write_reports(options, results, counts, rules, shell_version)
    return baseline_verdict(counts, read_baseline(options.baseline))


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
