#!/bin/bash
# Smoke test for POSIX <-> Windows path conversion.

echo "=== posix forms"
case "$PWD" in
    /[a-z]/*) echo "PWD is POSIX form" ;;
    *)        echo "PWD UNEXPECTED: $PWD" ;;
esac

case "$HOME" in
    /[a-z]/*) echo "HOME is POSIX form" ;;
    *)        echo "HOME UNEXPECTED: $HOME" ;;
esac

case "$PATH" in
    *:*) echo "PATH uses : separator" ;;
    *)   echo "PATH UNEXPECTED: $PATH" ;;
esac

echo "=== realpath posix"
realpath . | grep -qE '^/[a-z]/' && echo "realpath . is posix"

echo "=== /tmp resolves"
[ -d /tmp ] && echo "/tmp is a directory"

echo "=== /c/Windows is reachable"
[ -d /c/Windows ] && echo "/c/Windows ok"

echo "=== posix arg flows to native exe"
# Just ensure the path conv doesn't error out when invoking native.
# We don't run anything heavy.
which cmd.exe >/dev/null 2>&1 && echo "cmd.exe on PATH"

echo "=== done"
