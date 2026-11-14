#!/bin/bash
# $'...' quoting, \u escapes, printf %q and %z, and xpg_echo, as bash does
# them.

hex() {
	local s=$1 i
	for ((i = 0; i < ${#s}; i++)); do printf '%02x ' "'${s:i:1}"; done
	echo "(${#s})"
}

echo "=== 1. \$'...' inside double quotes stays literal"
x=1
echo "$'notansi' \$x ${x}"

echo "=== 2. a NUL ends the string"
hex $'a\0b'
hex $'c\x00d'
hex $'e\000f'

echo "=== 3. \\u and \\U"
echo $'u:\u0041 U:\U00000042'
LC_ALL=C.UTF-8
echo $'utf8:\u00e9' | od -An -tx1
printf 'printf:\u00e9\n' | od -An -tx1
echo -e 'echo:\U0001F600' | od -An -tx1
LC_ALL=C
echo $'c:\u00e9 \U0001F600'
printf 'c:\u00e9 \u\n'
unset LC_ALL
printf 'A\U00000042\n'

echo "=== 4. printf %q quotes control characters as \$'...'"
printf '%q\n' 'simple' 'with space' $'tab\there' $'nl\n' $'esc\e' $'bell\a'

echo "=== 5. printf rejects an unknown conversion"
printf 'before %z after\n' x
echo "rc=$?"
printf 'before %y after\n'
echo "rc=$?"
printf 'end %'
echo "rc=$?"

echo "=== 6. xpg_echo"
shopt -s xpg_echo
echo 'a\tb'
echo -E 'kept\tas is'
shopt -u xpg_echo
echo 'a\tb'
