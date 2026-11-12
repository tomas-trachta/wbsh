#!/bin/bash
# Arguments to native programs arrive byte for byte, whatever they contain.
# Wrong quoting on the way to CreateProcess turns data into extra arguments.
. "$STRESS_ROOT/lib/check.sh"

args=('a b' 'a"b' 'a\' 'a\\"b' '' 'x%PATH%y' '!x!' 'ž€😀' '*' '-c' $'a\tb' '\server\share'
      'C:\dir\' '"' '\"' 'a\\\\"' "it's" '$(echo hi)' '&|<>^' ' lead' 'trail ' '--' '"a b"')

severity high
py=$(command -v python 2>/dev/null || command -v python3 2>/dev/null)
case "$py" in
    *WindowsApps*|"") py="" ;;
esac
if [ -n "$py" ]; then
    for a in "${args[@]}"; do
        got=$("$py" -c 'import sys; print(len(sys.argv) - 1, sys.argv[1].encode("utf-8").hex())' "$a" end)
        want="2 $(printf '%s' "$a" | od -An -tx1 | tr -d ' \n')"
        expect_eq "python receives $(printf '%q' "$a") unchanged" "$got" "$want"
    done
else
    skip "python not found; native argv check skipped"
fi

for a in "${args[@]}"; do
    got=$("$STRESS_WBSH" -r -c 'printf "%s|%s" "$1" "$#"' _ "$a" < /dev/null)
    expect_eq "child wbsh receives $(printf '%q' "$a") unchanged" "$got" "$a|1"
done

severity low
got=$("$py" -c 'import sys; print(sys.argv[1])' /c/Windows 2>/dev/null)
[ -n "$py" ] && note "a POSIX path argument reaches native programs as: $got (path conversion)"
finish
