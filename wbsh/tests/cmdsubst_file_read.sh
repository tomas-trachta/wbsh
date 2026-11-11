#!/bin/bash
# `$(< file)` reads the file, as bash does, instead of running an empty
# command (#291).

mkdir -p _run_tmp
printf 'abc\n' > _run_tmp/fr_one
printf 'a\nb\n\n\n' > _run_tmp/fr_lines
: > _run_tmp/fr_empty

echo "=== the spellings"
echo "[$(< _run_tmp/fr_one)] [$(<_run_tmp/fr_one)] [$( < _run_tmp/fr_one )] [$(0< _run_tmp/fr_one)]"
f=_run_tmp/fr_one
echo "quoted variable: [$(< "$f")]"

echo "=== trailing newlines are stripped, inner ones kept"
x=$(< _run_tmp/fr_lines)
printf '%s <%s>\n' "${#x}" "$x"

echo "=== an empty file"
e=$(< _run_tmp/fr_empty)
echo "[$e] rc=$?"
[[ -f "$f" && -z "$(< "$f")" ]] && echo "empty" || echo "not empty"

echo "=== a missing file"
m=$(< _run_tmp/fr_missing)
echo "rc=$? [$m]"

echo "=== anything more than the lone redirection runs as usual"
echo "[$(< _run_tmp/fr_one; echo more)]"
echo "[$(< _run_tmp/fr_one cat)]"

rm -f _run_tmp/fr_one _run_tmp/fr_lines _run_tmp/fr_empty
