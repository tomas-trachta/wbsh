#!/usr/bin/env python3
"""
Style checker for wbsh.

Mechanically enforces the subset of CONTRIBUTING.md's rules that can be
checked without parsing C++ properly:

  - Tabs (not spaces) for indentation.
  - No trailing whitespace.
  - LF line endings, with a trailing newline at EOF.
  - Line length <= 120 hard limit; warning at > 100.
  - No anonymous namespaces (`namespace { ... }`).
  - Every header has `#pragma once`.
  - Function bodies are at most 60 lines (heuristic; see below).

The function-length check is a heuristic. It strips comments and string
literals, then walks brace pairs and flags a `{...}` as a function body
when the text immediately before the `{` looks like an argument list
(closing `)` possibly followed by `const` / `noexcept` / `override` /
trailing return type). Lambdas with parenthesized parameter lists count
as functions too, which is fine — a 60-line lambda is also too long.

Usage:
    python tools/check_style.py
    python tools/check_style.py src/foo.cpp src/bar.h
    python tools/check_style.py --warnings-as-errors

Exit code: 0 if clean (warnings allowed), 1 if any errors, 2 on bad usage.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


MAX_FUNC_BODY_LINES = 60
MAX_LINE_LEN_HARD = 120
MAX_LINE_LEN_SOFT = 100
TAB_WIDTH = 4

SRC_EXTENSIONS = {".cpp", ".cc", ".cxx", ".h", ".hpp"}
DEFAULT_SCAN_ROOTS = ["src"]

CONTROL_KEYWORDS = frozenset({
    "if", "while", "for", "switch", "catch",
    "sizeof", "decltype", "alignof", "static_assert",
})


@dataclass
class Issue:
    path: Path
    line: int
    rule: str
    message: str
    severity: str  # "error" or "warning"

    def format(self) -> str:
        return f"{self.path}:{self.line}: {self.severity}: [{self.rule}] {self.message}"


def strip_comments_and_strings(text: str) -> str:
    """Replace string and comment contents with spaces, preserving newlines.

    Lets the structural checks pattern-match against code without false hits
    from strings like "namespace {" or commented-out code. Newlines are kept
    so line numbers stay aligned with the original.
    """
    out: list[str] = []
    i = 0
    n = len(text)
    state = "code"  # "code" | "sl" | "blk" | "string" | "char"
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == "code":
            if c == "/" and nxt == "/":
                state = "sl"
                out.append("  ")
                i += 2
                continue
            if c == "/" and nxt == "*":
                state = "blk"
                out.append("  ")
                i += 2
                continue
            if c == '"':
                state = "string"
                out.append(c)
                i += 1
                continue
            if c == "'":
                state = "char"
                out.append(c)
                i += 1
                continue
            out.append(c)
            i += 1
            continue
        if state == "sl":
            if c == "\n":
                state = "code"
                out.append("\n")
            else:
                out.append(" ")
            i += 1
            continue
        if state == "blk":
            if c == "*" and nxt == "/":
                state = "code"
                out.append("  ")
                i += 2
                continue
            out.append("\n" if c == "\n" else " ")
            i += 1
            continue
        if state == "string":
            if c == "\\" and nxt:
                out.append("  ")
                i += 2
                continue
            if c == '"':
                state = "code"
                out.append(c)
                i += 1
                continue
            out.append("\n" if c == "\n" else " ")
            i += 1
            continue
        # state == "char"
        if c == "\\" and nxt:
            out.append("  ")
            i += 2
            continue
        if c == "'":
            state = "code"
            out.append(c)
            i += 1
            continue
        out.append(" ")
        i += 1
    return "".join(out)


QUALIFIER_RE = re.compile(
    r"\s*(?:"
    r"const|mutable|volatile|"
    r"noexcept(?:\s*\([^)]*\))?|"
    r"override|final|"
    r"=\s*0|=\s*default|=\s*delete|"
    r"->\s*[\w:&*<>,\s]+|"
    r"throw\s*\([^)]*\)|"
    r"&{1,2}"
    r")\s*$"
)


def _strip_qualifiers(prefix: str) -> str:
    prev: str | None = None
    while prev != prefix:
        prev = prefix
        prefix = QUALIFIER_RE.sub("", prefix).rstrip()
    return prefix


def _opener_is_function(prefix: str) -> bool:
    """True iff `prefix` (text before a `{`) looks like a function header."""
    prefix = _strip_qualifiers(prefix.rstrip())
    if not prefix.endswith(")"):
        return False
    depth = 0
    i = len(prefix) - 1
    while i >= 0:
        if prefix[i] == ")":
            depth += 1
        elif prefix[i] == "(":
            depth -= 1
            if depth == 0:
                break
        i -= 1
    if depth != 0 or i < 0:
        return False
    before = prefix[:i].rstrip()
    m = re.search(r"(\w+)\s*$", before)
    if m and m.group(1) in CONTROL_KEYWORDS:
        return False
    return True


def find_function_bodies(stripped: str) -> Iterable[tuple[int, int]]:
    """Yield (open_line, close_line) for each `{...}` that opens a function.

    Both line numbers are 1-based and refer to lines in the original file.
    """
    last_break = 0
    stack: list[tuple[int, bool]] = []
    line = 1
    i = 0
    n = len(stripped)
    while i < n:
        c = stripped[i]
        if c == "\n":
            line += 1
        elif c == ";":
            last_break = i + 1
        elif c == "{":
            segment = stripped[last_break:i]
            stack.append((line, _opener_is_function(segment)))
            last_break = i + 1
        elif c == "}":
            if stack:
                open_line, is_func = stack.pop()
                if is_func:
                    yield (open_line, line)
            last_break = i + 1
        i += 1


ANON_NS_RE = re.compile(r"\bnamespace\s*\{")
PRAGMA_ONCE_RE = re.compile(r"^\s*#\s*pragma\s+once\b", re.MULTILINE)


def _visual_columns(line: str) -> int:
    col = 0
    for c in line:
        if c == "\t":
            col = (col // TAB_WIDTH + 1) * TAB_WIDTH
        else:
            col += 1
    return col


def check_file(path: Path) -> list[Issue]:
    issues: list[Issue] = []
    raw = path.read_bytes()
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as e:
        issues.append(Issue(path, 0, "encoding", f"not valid UTF-8: {e}", "error"))
        return issues

    if not raw.endswith(b"\n"):
        eof_line = max(1, raw.count(b"\n") + 1)
        issues.append(Issue(path, eof_line, "trailing-newline",
                            "missing trailing newline at EOF", "error"))
    if b"\r" in raw:
        first_cr = raw.find(b"\r")
        line_no = raw.count(b"\n", 0, first_cr) + 1
        issues.append(Issue(path, line_no, "line-endings",
                            "file uses CRLF line endings (use LF)", "error"))
        text = text.replace("\r\n", "\n").replace("\r", "\n")

    stripped_text = strip_comments_and_strings(text)

    lines = text.split("\n")
    stripped_lines = stripped_text.split("\n")
    if lines and lines[-1] == "":
        lines = lines[:-1]
        stripped_lines = stripped_lines[:-1]

    for idx, line in enumerate(lines, start=1):
        sline = stripped_lines[idx - 1] if idx - 1 < len(stripped_lines) else ""
        if line.rstrip(" \t") != line:
            issues.append(Issue(path, idx, "trailing-whitespace",
                                "trailing whitespace", "error"))
        # Indent check only applies to lines with real code; the leading-space
        # in Doxygen continuations (` * foo`) is not indentation.
        if sline.strip():
            leading = line[: len(line) - len(line.lstrip(" \t"))]
            if leading.startswith(" "):
                issues.append(Issue(path, idx, "indent-spaces",
                                    "line indented with spaces (use tabs)",
                                    "error"))
        cols = _visual_columns(line.rstrip())
        if cols > MAX_LINE_LEN_HARD:
            issues.append(Issue(path, idx, "line-too-long",
                                f"line is {cols} columns (limit {MAX_LINE_LEN_HARD})",
                                "error"))
        elif cols > MAX_LINE_LEN_SOFT:
            issues.append(Issue(path, idx, "line-soft-limit",
                                f"line is {cols} columns (soft limit {MAX_LINE_LEN_SOFT})",
                                "warning"))

    for m in ANON_NS_RE.finditer(stripped_text):
        line_no = stripped_text[: m.start()].count("\n") + 1
        issues.append(Issue(path, line_no, "anon-namespace",
                            "anonymous namespace (use `static` or a named "
                            "sub-namespace)", "error"))

    if path.suffix in (".h", ".hpp"):
        if not PRAGMA_ONCE_RE.search(text):
            issues.append(Issue(path, 1, "pragma-once",
                                "header missing `#pragma once`", "error"))

    for start, end in find_function_bodies(stripped_text):
        body_lines = end - start - 1
        if body_lines > MAX_FUNC_BODY_LINES:
            issues.append(Issue(path, start, "function-too-long",
                                f"function body is {body_lines} lines "
                                f"(limit {MAX_FUNC_BODY_LINES}; closes at line {end})",
                                "error"))

    return issues


def collect_paths(args_paths: list[str], repo_root: Path) -> list[Path]:
    if not args_paths:
        roots = [repo_root / r for r in DEFAULT_SCAN_ROOTS]
    else:
        roots = [Path(p) for p in args_paths]
    out: list[Path] = []
    for r in roots:
        if r.is_dir():
            for ext in SRC_EXTENSIONS:
                out.extend(sorted(r.rglob(f"*{ext}")))
        elif r.is_file():
            if r.suffix in SRC_EXTENSIONS:
                out.append(r)
            else:
                print(f"check_style: skipping {r} (not a C/C++ source file)",
                      file=sys.stderr)
        else:
            print(f"check_style: no such file or directory: {r}", file=sys.stderr)
    seen: set[Path] = set()
    uniq: list[Path] = []
    for p in out:
        rp = p.resolve()
        if rp in seen:
            continue
        seen.add(rp)
        uniq.append(p)
    return uniq


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Mechanical style checker for wbsh.")
    ap.add_argument("paths", nargs="*",
                    help="files or directories to check (default: src/)")
    ap.add_argument("--warnings-as-errors", action="store_true",
                    help="treat warnings as failures")
    ns = ap.parse_args(argv)

    repo_root = Path(__file__).resolve().parent.parent
    paths = collect_paths(ns.paths, repo_root)
    if not paths:
        print("check_style: no source files found", file=sys.stderr)
        return 2

    all_issues: list[Issue] = []
    for p in paths:
        all_issues.extend(check_file(p))

    all_issues.sort(key=lambda i: (str(i.path), i.line, i.rule))
    for issue in all_issues:
        print(issue.format())

    n_errors = sum(1 for i in all_issues if i.severity == "error")
    n_warnings = sum(1 for i in all_issues if i.severity == "warning")
    print(f"\nchecked {len(paths)} files: {n_errors} errors, {n_warnings} warnings",
          file=sys.stderr)

    if n_errors > 0:
        return 1
    if ns.warnings_as_errors and n_warnings > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
