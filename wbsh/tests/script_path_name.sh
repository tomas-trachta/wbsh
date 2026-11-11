#!/bin/bash
# A script's `$0` is the path as typed, or the POSIX form of the file found
# on PATH, and `wbsh -r` opens a POSIX absolute path (#291).

WBSH=../../x64/Release/wbsh.exe
[ -x "$WBSH" ] || WBSH=../../x64/Debug/wbsh.exe

mkdir -p _run_tmp/spn/bin
root=$(cd _run_tmp/spn && pwd)
printf '#!/bin/sh\necho "$0"\n' > _run_tmp/spn/bin/show0
chmod +x _run_tmp/spn/bin/show0

shape() {
    local name=$1
    case $name in
        *\*) echo "$2: has a backslash: $name" ;;
        /*)   [ "$name" -ef "$root/bin/show0" ] && echo "$2: POSIX absolute, same file" ;;
        *)    echo "$2: as typed: $name" ;;
    esac
}

echo "=== relative path"
(cd _run_tmp/spn && shape "$(./bin/show0)" "./bin/show0")
(cd _run_tmp/spn && shape "$(bin/show0)" "bin/show0")

echo "=== absolute path"
shape "$("$root/bin/show0")" "absolute"

echo "=== found on PATH"
shape "$(PATH="$root/bin:$PATH"; show0)" "PATH lookup"

echo "=== dirname \$0 finds the install directory"
printf '#!/bin/sh\ncd -P "$(dirname "$0")/.." && pwd\n' > _run_tmp/spn/bin/home
chmod +x _run_tmp/spn/bin/home
[ "$(PATH="$root/bin:$PATH"; home)" -ef "$root" ] && echo "home found"

echo "=== wbsh -r takes a POSIX absolute path and keeps it as \$0"
out=$(MSYS_NO_PATHCONV=1 "$WBSH" -r "$root/bin/show0")
[ "$out" = "$root/bin/show0" ] && echo "same spelling"

rm -rf _run_tmp/spn
