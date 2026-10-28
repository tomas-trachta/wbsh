#!/bin/bash
# A TEMP directory and file names with non-ASCII characters work: wbsh
# talks to Windows through the wide (UTF-16) APIs, not the ANSI ones.

if [ -x ../../x64/Release/wbsh.exe ]; then
	W=../../x64/Release/wbsh.exe
else
	W=../../x64/Debug/wbsh.exe
fi

mkdir -p _run_tmp
D="$PWD/_run_tmp/tmpž_日本"
rm -rf "$D"
mkdir -p "$D"
P=_run_tmp/unicode_probe.sh
cat > $P <<'EOF'
x=$(echo captured); echo "[$x]"
echo hi > /tmp/unicode_probe_file && cat /tmp/unicode_probe_file
rm -f /tmp/unicode_probe_file
f=$(mktemp) && [ -f "$f" ] && echo "mktemp ok" && rm -f "$f"
d=$(mktemp -d) && [ -d "$d" ] && echo "mktemp -d ok" && rmdir "$d"
cat <(echo procsub)
cat <<END
heredoc $(echo inside)
END
EOF

echo "=== 1. TEMP with diacritics"
D_WIN=$(cd "$D" && pwd -W)
TMP="$D_WIN" TEMP="$D_WIN" TMPDIR="$D_WIN" $W -r $P
echo "rc=$?"

echo "=== 2. file tests on a non-ASCII name"
F="$D/soubor_ž_文字.txt"
printf 'obsah\n' > "$F"
[ -s "$F" ] && echo "non-empty"
[ -x "$F" ] || echo "not executable"
[ -f "$F" ] && echo "regular file"
[ "$F" -nt "$D/missing" ] && echo "newer than a missing file"
stat "$F" | grep -c Size
wc -c < "$F"
mkdir -p "$D/adresář_ディレクトリ"
[ -d "$D/adresář_ディレクトリ" ] && echo "directory"
ls "$D"
ln "$F" "$D/link_文字" && cat "$D/link_文字"
chmod a-w "$F"; [ -w "$F" ] || echo "read-only"
chmod u+w "$F"; [ -w "$F" ] && echo "writable"

echo "=== 3. scripts and sources with non-ASCII names"
S="$D/skript_č_脚本.sh"
echo 'echo "ran $0"' > "$S"
$W -r "$S" | sed 's#.*[/\]##'
. "$S" | sed 's#ran .*/#ran #'
rm -rf "$D"
