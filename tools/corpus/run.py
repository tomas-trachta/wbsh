#!/usr/bin/env python3
"""
Real-world corpus runner: fetches the bash frameworks and init scripts listed
in manifest.json at their pinned commits, sources every entry under one or
more shells, non-interactively and with -i, and records each run as ok,
slow, fails or hangs.

Usage:
    python -I tools/corpus/run.py --shell wbsh=x64/Release/wbsh.exe \\
        --shell "bash=C:/Program Files/Git/bin/bash.exe" \\
        --clone-root C:/scratch/corpus-src --markdown snapshot.md --json corpus.json

The first --shell is the one under test; later ones are references. Assets
are fetched with a depth-1 git fetch of the pinned commit into --clone-root
(created empty if missing). Nothing from an asset is executed except by
sourcing the listed entry files under the shells, each in a fresh working
directory with HOME pointed at a fresh empty directory and stdin closed.
See README.md for the manifest format and the result categories.

Exit status: 0 when the run finished, 2 on bad usage or a failed fetch.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import contextlib
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
DEFAULT_MANIFEST = HERE / "manifest.json"

DEFAULT_TIMEOUT = 20.0
DEFAULT_SLOW = 1.0
STDERR_EXCERPT = 300
CHECK_FAILED_STATUS = 97

MODES = ("script", "interactive")
INTERACTIVE_ARGUMENTS = ("--norc", "--noediting", "-i")
INTERACTIVE_PROBE = b"case $- in *i*) echo interactive-yes ;; esac\n"
CATEGORY_ORDER = ("ok", "slow", "fails", "hangs")

SCRUBBED_PREFIXES = ("WBSH_", "WBSHTERM_", "BASH_FUNC_")
SCRUBBED_VARIABLES = (
    "BASH_ENV", "ENV", "CDPATH", "GLOBIGNORE", "POSIXLY_CORRECT", "SHELLOPTS", "BASHOPTS",
    "PROMPT_COMMAND", "PS1", "HISTFILE", "INPUTRC", "OSH", "BASH_IT", "NVM_DIR", "SDKMAN_DIR",
    "ASDF_DIR", "ASDF_DATA_DIR", "BASH_COMPLETION_USER_DIR", "XDG_CONFIG_HOME", "XDG_DATA_HOME",
    "XDG_CACHE_HOME", "XDG_STATE_HOME",
)

INTERACTIVE_NOISE = re.compile(
    r"^(?:\S+: cannot set terminal process group \(-?\d+\): .*|\S+: no job control in this shell|exit"
    r"|\S+: bind: warning: line editing not enabled)$",
    re.MULTILINE,
)

SHELL_PREFIX = re.compile(r"^(?:(?:wbsh|bash|sh|bash\.exe): )+")
SOURCE_LOCATION = re.compile(r"\S+: line \d+: ")
CAUSE_ROWS = 12

WINDOWS_BELOW_NORMAL = 0x00004000


@dataclass
class Shell:
    name: str
    path: str
    version: str = ""


@dataclass
class Entry:
    asset: str
    name: str
    file: str
    setup: list[str]
    check: str
    modes: list[str]
    globbed: bool = False


@dataclass
class Asset:
    name: str
    root: Path
    description: str
    pin: str
    writable: bool = False


@dataclass
class Run:
    category: str
    status: str
    seconds: float
    stderr: str


@dataclass
class Outcome:
    entry: Entry
    runs: dict[tuple[str, str], Run] = field(default_factory=dict)


def git(arguments: list[str], cwd: Path | None = None) -> str:
    completed = subprocess.run(
        ["git", "-c", "advice.detachedHead=false", *arguments], cwd=cwd,
        stdin=subprocess.DEVNULL, capture_output=True, timeout=600,
    )
    if completed.returncode != 0:
        message = completed.stderr.decode("utf-8", errors="replace").strip()
        raise SystemExit(f"git {' '.join(arguments)} failed: {message}")
    return completed.stdout.decode("utf-8", errors="replace").strip()


def checked_out_commit(directory: Path) -> str | None:
    if not (directory / ".git").exists():
        return None
    completed = subprocess.run(
        ["git", "rev-parse", "HEAD"], cwd=directory, stdin=subprocess.DEVNULL, capture_output=True)
    return completed.stdout.decode().strip() if completed.returncode == 0 else None


def fetch_pinned(spec: dict, directory: Path) -> None:
    if checked_out_commit(directory) == spec["commit"]:
        git(["clean", "-fdxq"], directory)
        git(["checkout", "--quiet", "--", "."], directory)
        return
    if directory.exists():
        shutil.rmtree(directory)
    directory.mkdir(parents=True)

    git(["init", "--quiet"], directory)
    git(["remote", "add", "origin", spec["repo"]], directory)
    sparse = spec.get("sparse")
    if sparse:
        git(["sparse-checkout", "set", "--no-cone", *sparse], directory)
    filter_arguments = ["--filter=blob:none"] if sparse else []
    git(["fetch", "--quiet", "--depth", "1", *filter_arguments, "origin", spec["commit"]], directory)
    git(["checkout", "--quiet", "--detach", "FETCH_HEAD"], directory)

    if checked_out_commit(directory) != spec["commit"]:
        raise SystemExit(f"{spec['name']}: checked out commit is not {spec['commit']}")


def fetch_assets(manifest: dict, clone_root: Path, patterns: list[str]) -> list[Asset]:
    assets = []
    for spec in manifest["assets"]:
        if patterns and not any(fnmatch.fnmatch(spec["name"], pattern) for pattern in patterns):
            continue
        directory = clone_root / spec["name"]
        print(f"fetching {spec['name']} @ {spec['ref']}", file=sys.stderr, flush=True)
        fetch_pinned(spec, directory)
        pin = f"{spec['ref']} (`{spec['commit'][:12]}`)"
        assets.append(Asset(spec["name"], directory, spec.get("description", ""), pin, spec.get("writable", False)))
    return assets


def generate_init_scripts(manifest: dict, directory: Path, patterns: list[str]) -> list[Asset]:
    assets = []
    for spec in manifest.get("generated", []):
        if patterns and not any(fnmatch.fnmatch(spec["name"], pattern) for pattern in patterns):
            continue
        tool = shutil.which(spec["tool"])
        if tool is None:
            print(f"skipping {spec['name']}: {spec['tool']} is not installed", file=sys.stderr)
            continue
        assets.append(generate_one(spec, tool, directory))
    return assets


def generate_one(spec: dict, tool: str, directory: Path) -> Asset:
    root = directory / spec["name"]
    root.mkdir(parents=True, exist_ok=True)
    completed = subprocess.run([tool, *spec["args"]], stdin=subprocess.DEVNULL, capture_output=True, timeout=60)
    (root / "init.bash").write_bytes(completed.stdout)
    version = tool_version(tool)
    return Asset(spec["name"], root, spec.get("description", ""), f"`{spec['tool']} {version}`")


def tool_version(tool: str) -> str:
    completed = subprocess.run([tool, "--version"], stdin=subprocess.DEVNULL, capture_output=True, timeout=30)
    lines = completed.stdout.decode("utf-8", errors="replace").strip().splitlines()
    return lines[0] if lines else "?"


def entry_specs(manifest: dict, asset: Asset) -> list[dict]:
    for section in ("assets", "generated"):
        for spec in manifest.get(section, []):
            if spec["name"] == asset.name:
                return spec["entries"]
    return []


def expand_entry(asset: Asset, spec: dict) -> list[Entry]:
    modes = spec.get("modes", list(MODES))
    setup = spec.get("setup", [])
    check = spec.get("check", "")
    if "glob" not in spec:
        return [Entry(asset.name, spec["name"], spec["file"], setup, check, modes)]

    excluded = spec.get("exclude", [])
    entries = []
    for path in globbed_files(asset.root, spec["glob"]):
        relative = path.relative_to(asset.root).as_posix()
        if not any(fnmatch.fnmatch(relative, pattern) for pattern in excluded):
            entries.append(Entry(asset.name, relative, relative, setup, check, modes, globbed=True))
    return entries


def globbed_files(root: Path, patterns: list[str]) -> list[Path]:
    found = set()
    for pattern in patterns:
        found.update(path for path in root.glob(pattern) if path.is_file())
    return sorted(found)


def collect_entries(manifest: dict, assets: list[Asset]) -> list[Entry]:
    entries = []
    for asset in assets:
        for spec in entry_specs(manifest, asset):
            entries += expand_entry(asset, spec)
    return entries


def shell_path(path: Path) -> str:
    return str(path).replace("\\", "/")


def single_quoted(text: str) -> str:
    return "'" + text.replace("'", "'\\''") + "'"


def driver_script(entry: Entry, asset_root: Path) -> str:
    lines = [f"ASSET={single_quoted(shell_path(asset_root))}", *entry.setup]
    lines.append(f". {single_quoted(shell_path(asset_root / entry.file))}")
    lines.append("corpus_status=$?")
    if entry.check:
        message = single_quoted(f"corpus: check failed: {entry.check}")
        lines.append(f"{{ {entry.check}; }} || {{ echo {message} >&2; exit {CHECK_FAILED_STATUS}; }}")
    lines.append("exit $corpus_status")
    return "{\n" + "\n".join(lines) + "\n}\n"


def is_scrubbed(key: str) -> bool:
    upper = key.upper()
    return upper in SCRUBBED_VARIABLES or upper.startswith(SCRUBBED_PREFIXES) or upper.startswith("LC_")


def build_environment(home: Path) -> dict[str, str]:
    env = {key: value for key, value in os.environ.items() if not is_scrubbed(key)}
    env["HOME"] = str(home)
    env["TERM"] = "xterm-256color"
    env["LANG"] = "C.UTF-8"
    env["PS1"] = ""
    env["PS2"] = ""
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


def execute(command: list[str], workdir: Path, env: dict[str, str], stdin, options) -> tuple[str, bytes, float]:
    started = time.perf_counter()
    process = subprocess.Popen(
        command, cwd=workdir, env=env,
        stdin=stdin, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
        creationflags=creation_flags(options.low_priority), start_new_session=os.name != "nt",
    )
    try:
        _, stderr = process.communicate(timeout=options.timeout)
        status = str(process.returncode)
    except subprocess.TimeoutExpired:
        kill_tree(process)
        _, stderr = process.communicate()
        status = "timeout"
    return status, stderr, time.perf_counter() - started


def run_shell(shell: Shell, mode: str, driver: Path, workdir: Path, home: Path, options) -> tuple[str, bytes, float]:
    env = build_environment(home)
    if mode == "script":
        return execute([shell.path, shell_path(driver)], workdir, env, subprocess.DEVNULL, options)
    with driver.open("rb") as stdin:
        return execute([shell.path, *INTERACTIVE_ARGUMENTS], workdir, env, stdin, options)


def clean_stderr(raw: bytes, mode: str) -> str:
    text = raw.decode("utf-8", errors="replace").replace("\r\n", "\n")
    if mode == "interactive":
        text = INTERACTIVE_NOISE.sub("", text)
    return "\n".join(line for line in text.splitlines() if line.strip())


def path_spellings(path: Path) -> list[str]:
    forward = shell_path(path)
    spellings = {str(path), forward}
    if re.match(r"^[A-Za-z]:/", forward):
        spellings.add(f"/{forward[0].lower()}{forward[2:]}")
    return sorted(spellings, key=len, reverse=True)


def anonymize(text: str, replacements: dict[Path, str]) -> str:
    for path, name in replacements.items():
        for spelling in path_spellings(path):
            text = re.sub(re.escape(spelling), lambda _: name, text, flags=re.IGNORECASE)
    return text


def categorize(status: str, stderr: str, seconds: float, slow: float) -> str:
    if status == "timeout":
        return "hangs"
    if status != "0" or stderr.strip():
        return "fails"
    if seconds > slow:
        return "slow"
    return "ok"


def prepare_job_dir(scratch: Path, index: int) -> tuple[Path, Path]:
    workdir = scratch / f"job{index}"
    if workdir.exists():
        shutil.rmtree(workdir, ignore_errors=True)
    home = workdir / "home"
    home.mkdir(parents=True)
    return workdir, home


def private_copy(asset: Asset, workdir: Path) -> Path:
    if not asset.writable:
        return asset.root
    copy = workdir / "asset"
    shutil.copytree(asset.root, copy, ignore=shutil.ignore_patterns(".git"), symlinks=True)
    return copy


def run_job(shell: Shell, mode: str, entry: Entry, asset: Asset, scratch: Path, index: int, options) -> Run:
    workdir, home = prepare_job_dir(scratch, index)
    asset_root = private_copy(asset, workdir)
    driver = workdir / "driver.sh"
    driver.write_bytes(driver_script(entry, asset_root).encode("utf-8"))

    status, raw_stderr, seconds = run_shell(shell, mode, driver, workdir, home, options)
    stderr = anonymize(clean_stderr(raw_stderr, mode), {asset_root: "$ASSET", workdir: "$WORK"})
    shutil.rmtree(workdir, ignore_errors=True)
    return Run(categorize(status, stderr, seconds, options.slow), status, seconds, stderr)


def planned_jobs(entries: list[Entry], shells: list[Shell]) -> list[tuple[Entry, Shell, str]]:
    return [(entry, shell, mode) for entry in entries for mode in entry.modes for shell in shells]


def run_corpus(entries, shells, assets: dict[str, Asset], scratch: Path, options) -> list[Outcome]:
    outcomes = {id(entry): Outcome(entry) for entry in entries}
    jobs = planned_jobs(entries, shells)

    def one(index: int) -> None:
        entry, shell, mode = jobs[index]
        run = run_job(shell, mode, entry, assets[entry.asset], scratch, index, options)
        outcomes[id(entry)].runs[(shell.name, mode)] = run
        if index % 50 == 0:
            print(f"{index + 1}/{len(jobs)} runs", file=sys.stderr, flush=True)

    with concurrent.futures.ThreadPoolExecutor(max_workers=options.jobs) as pool:
        list(pool.map(one, range(len(jobs))))
    return list(outcomes.values())


def interactive_flag_set(shell: Shell) -> bool:
    with tempfile.TemporaryDirectory(prefix="corpus-probe-") as directory:
        completed = subprocess.run(
            [shell.path, *INTERACTIVE_ARGUMENTS], cwd=directory, input=INTERACTIVE_PROBE,
            env=build_environment(Path(directory)), capture_output=True, timeout=30,
        )
    return b"interactive-yes" in completed.stdout


def shell_version(shell: Shell) -> str:
    try:
        completed = subprocess.run(
            [shell.path, "-c", 'printf %s "$BASH_VERSION"'],
            stdin=subprocess.DEVNULL, capture_output=True, timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired):
        return "?"
    return completed.stdout.decode("utf-8", errors="replace").strip() or "?"


def tally(outcomes: list[Outcome], shell: str, mode: str) -> dict[str, int]:
    counts = {category: 0 for category in CATEGORY_ORDER}
    for outcome in outcomes:
        run = outcome.runs.get((shell, mode))
        if run:
            counts[run.category] += 1
    return counts


def regressions(outcomes: list[Outcome], tested: str, reference: str, mode: str) -> list[Outcome]:
    found = []
    for outcome in outcomes:
        mine, theirs = outcome.runs.get((tested, mode)), outcome.runs.get((reference, mode))
        if mine and theirs and mine.category in ("fails", "hangs") and theirs.category in ("ok", "slow"):
            found.append(outcome)
    return found


def counts_cell(counts: dict[str, int]) -> str:
    total = sum(counts.values())
    good = counts["ok"] + counts["slow"]
    breakdown = ", ".join(f"{counts[category]} {category}" for category in CATEGORY_ORDER)
    return f"{good}/{total} ({breakdown})"


def excerpt(text: str) -> str:
    flat = " ".join(line.strip() for line in text.strip().splitlines() if line.strip())
    flat = flat.replace("|", "\\|").replace("`", "'")
    return flat[:STDERR_EXCERPT] + ("..." if len(flat) > STDERR_EXCERPT else "")


def summary_rows(outcomes, shells, modes) -> list[str]:
    rows = ["| shell | mode | sourced cleanly |", "| --- | --- | --- |"]
    for shell in shells:
        for mode in modes:
            rows.append(f"| {shell.name} | {mode} | {counts_cell(tally(outcomes, shell.name, mode))} |")
    return rows


def asset_rows(outcomes, assets, shells, modes) -> list[str]:
    header = "| asset | pin | entries | " + " | ".join(f"{s.name} {m}" for s in shells for m in modes) + " |"
    rows = [header, "| --- | --- | ---: |" + " ---: |" * (len(shells) * len(modes))]
    for asset in assets:
        mine = [outcome for outcome in outcomes if outcome.entry.asset == asset.name]
        cells = []
        for shell in shells:
            for mode in modes:
                counts = tally(mine, shell.name, mode)
                cells.append(f"{counts['ok'] + counts['slow']}/{sum(counts.values())}")
        rows.append(f"| {asset.name} | {asset.pin} | {len(mine)} | " + " | ".join(cells) + " |")
    return rows


def entry_rows(outcomes, shells, modes, limit_to=None) -> list[str]:
    header = "| entry | " + " | ".join(f"{s.name} {m}" for s in shells for m in modes) + " | first stderr line |"
    rows = [header, "| --- |" + " --- |" * (len(shells) * len(modes) + 1)]
    for outcome in outcomes:
        if limit_to is not None and outcome not in limit_to:
            continue
        cells, message = [], ""
        for shell in shells:
            for mode in modes:
                run = outcome.runs.get((shell.name, mode))
                cells.append(run_cell(run))
                if run and not message and run.category == "fails" and shell is shells[0]:
                    message = excerpt(run.stderr) or f"exit {run.status}"
        rows.append(f"| {outcome.entry.asset}: `{outcome.entry.name}` | " + " | ".join(cells) + f" | {message} |")
    return rows


def run_cell(run: Run | None) -> str:
    if run is None:
        return "-"
    if run.category == "hangs":
        return "hangs"
    return f"{run.category} {run.seconds:.2f}s"


def notable(outcomes: list[Outcome], shells: list[Shell]) -> list[Outcome]:
    tested = shells[0].name
    return [
        outcome for outcome in outcomes
        if (not outcome.entry.globbed or tested_hangs(outcome, tested))
        and any(run.category != "ok" for (name, _), run in outcome.runs.items() if name == tested)
    ]


def tested_hangs(outcome: Outcome, tested: str) -> bool:
    return any(run.category == "hangs" for (name, _), run in outcome.runs.items() if name == tested)


def failure_cause(run: Run) -> str:
    lines = [line for line in run.stderr.splitlines() if line.strip()]
    if not lines:
        return f"exit status {run.status}" if run.category == "fails" else run.category
    cause = SHELL_PREFIX.sub("", lines[0].strip())
    cause = SOURCE_LOCATION.sub("<file>: line N: ", cause)
    return excerpt(cause)


def cause_rows(outcomes: list[Outcome], shell: str, limit: int = CAUSE_ROWS) -> list[str]:
    counts: dict[str, int] = {}
    examples: dict[str, str] = {}
    for outcome in outcomes:
        for (name, _), run in outcome.runs.items():
            if name == shell and run.category in ("fails", "hangs"):
                cause = failure_cause(run)
                counts[cause] = counts.get(cause, 0) + 1
                examples.setdefault(cause, f"{outcome.entry.asset}: `{outcome.entry.name}`")
    ranked = sorted(counts.items(), key=lambda item: -item[1])[:limit]
    rows = ["| runs | first stderr line (normalized) | example |", "| ---: | --- | --- |"]
    rows += [f"| {count} | {cause} | {examples[cause]} |" for cause, count in ranked]
    return rows


def markdown_report(outcomes, assets, shells, modes, interactive, options) -> str:
    lines = [
        "| shell | version | `-i` on piped stdin sets `i` in `$-` |",
        "| --- | --- | --- |",
        *[f"| {s.name} | `{s.version}` | {'yes' if interactive[s.name] else '**no**'} |" for s in shells],
        "",
        f"Timeout {options.timeout:.0f} s, slow above {options.slow:.1f} s. "
        f"\"Sourced cleanly\" counts ok + slow: exit status 0 and nothing on stderr.",
        "",
        *summary_rows(outcomes, shells, modes),
        "",
        "## Per asset",
        "",
        *asset_rows(outcomes, assets, shells, modes),
    ]
    if len(shells) > 1:
        lines += regression_section(outcomes, shells, modes)
    for shell in shells:
        lines += ["", f"## Most common failures under {shell.name}", "", *cause_rows(outcomes, shell.name)]
    lines += [
        "",
        f"## Top-level entries where {shells[0].name} is not plain ok",
        "",
        "Per-file results for the globbed plugins and completions are in the JSON.",
        "",
        *entry_rows(outcomes, shells, modes, notable(outcomes, shells)),
    ]
    return "\n".join(lines) + "\n"


def regression_section(outcomes, shells, modes) -> list[str]:
    lines = ["", f"## Fails under {shells[0].name}, clean under {shells[1].name}", ""]
    for mode in modes:
        found = regressions(outcomes, shells[0].name, shells[1].name, mode)
        lines.append(f"- {mode}: {len(found)} entries")
    return lines


def json_report(outcomes, assets, shells, interactive) -> dict:
    return {
        "recorded": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "shells": [{"name": s.name, "path": s.path, "version": s.version,
                    "interactive_flag": interactive[s.name]} for s in shells],
        "assets": [{"name": a.name, "pin": a.pin} for a in assets],
        "entries": [
            {
                "asset": outcome.entry.asset,
                "entry": outcome.entry.name,
                "runs": [
                    {"shell": shell, "mode": mode, "category": run.category, "status": run.status,
                     "seconds": round(run.seconds, 3), "stderr": run.stderr[:2000]}
                    for (shell, mode), run in outcome.runs.items()
                ],
            }
            for outcome in outcomes
        ],
    }


def write_outputs(options, outcomes, assets, shells, interactive) -> None:
    modes = [mode for mode in MODES if any(mode in outcome.entry.modes for outcome in outcomes)]
    report = markdown_report(outcomes, assets, shells, modes, interactive, options)
    print("\n".join(summary_rows(outcomes, shells, modes)))
    if options.markdown:
        Path(options.markdown).write_text(report, encoding="utf-8")
    if options.json:
        document = json_report(outcomes, assets, shells, interactive)
        Path(options.json).write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")


def parse_shell(spec: str) -> Shell:
    name, separator, path = spec.partition("=")
    if not separator or not name or not path:
        raise argparse.ArgumentTypeError(f"expected NAME=PATH, got {spec!r}")
    resolved = str(Path(path).resolve()) if Path(path).is_file() else shutil.which(path)
    if not resolved:
        raise argparse.ArgumentTypeError(f"shell not found: {path}")
    return Shell(name, resolved)


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Source real-world bash assets under shells.")
    parser.add_argument("--shell", type=parse_shell, action="append", required=True, metavar="NAME=PATH",
                        help="a shell to run; the first is the one under test")
    parser.add_argument("--clone-root", required=True, help="directory the assets are fetched into")
    parser.add_argument("--manifest", default=str(DEFAULT_MANIFEST), help="asset manifest (JSON)")
    parser.add_argument("--filter", action="append", default=[], help="glob on asset names, repeatable")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT, help="seconds per run")
    parser.add_argument("--slow", type=float, default=DEFAULT_SLOW, help="seconds above which a run is slow")
    parser.add_argument("--jobs", type=int, default=2, help="runs in parallel")
    parser.add_argument("--json", help="write every run as JSON")
    parser.add_argument("--markdown", help="write the markdown snapshot")
    parser.add_argument("--low-priority", action="store_true", help="run below normal priority")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    options = parse_arguments(argv)
    if options.low_priority:
        lower_own_priority()
    manifest = json.loads(Path(options.manifest).read_text(encoding="utf-8"))
    shells = options.shell
    for shell in shells:
        shell.version = shell_version(shell)
    interactive = {shell.name: interactive_flag_set(shell) for shell in shells}

    clone_root = Path(options.clone_root).resolve()
    clone_root.mkdir(parents=True, exist_ok=True)
    assets = fetch_assets(manifest, clone_root, options.filter)
    assets += generate_init_scripts(manifest, clone_root / "generated", options.filter)
    entries = collect_entries(manifest, assets)

    scratch = Path(tempfile.mkdtemp(prefix="corpus-"))
    outcomes = run_corpus(entries, shells, {asset.name: asset for asset in assets}, scratch, options)
    shutil.rmtree(scratch, ignore_errors=True)

    write_outputs(options, outcomes, assets, shells, interactive)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
