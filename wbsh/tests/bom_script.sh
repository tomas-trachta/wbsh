#!/bin/bash
# A UTF-8 byte order mark at the start of a script is skipped (Windows
# editors write one; bash fails on it). One in the middle of a line stays.

WBSH=../../x64/Release/wbsh.exe
[ -x "$WBSH" ] || WBSH=../../x64/Debug/wbsh.exe
D=$(mktemp -d) || exit 1
printf '\xef\xbb\xbfecho bom-ok\r\necho line $LINENO\r\n' > "$D/bom.sh"
printf 'echo x\xef\xbb\xbfy\n' > "$D/mid.sh"

"$WBSH" -r "$D/bom.sh"
. "$D/bom.sh"
"$WBSH" -r < "$D/bom.sh"
"$WBSH" -r "$D/mid.sh" | od -An -c
rm -rf "$D"
