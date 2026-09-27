#!/bin/bash
# Smoke test for sed (wbsh's sed currently supports s/PAT/REPL/[g] only).

echo "=== substitution (single)"
echo "hello world" | sed 's/world/wbsh/'
echo "aaaa" | sed 's/a/B/'

echo "=== substitution (global)"
echo "aaaa" | sed 's/a/B/g'
echo "one,two,three" | sed 's/,/ /g'

echo "=== multi expr (-e)"
echo "abcdef" | sed -e 's/a/A/' -e 's/c/C/'
echo "abcdef" | sed -e 's/a/A/g' -e 's/A/X/g'

echo "=== regex metachars"
printf 'one\ntwo\nthree\n' | sed 's/^t/T/'
echo "abc123" | sed 's/[0-9]/#/g'
echo "abc def" | sed 's/  */-/g'

echo "=== empty replacement"
echo "abXcdXef" | sed 's/X//g'

echo "=== done"
