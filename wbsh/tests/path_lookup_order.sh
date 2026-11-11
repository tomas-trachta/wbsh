#!/bin/bash
# A name with a path runs the script or program it names, as in bash, even
# with a `.bat` beside it. A PATH lookup keeps the Windows order and finds
# `.exe`, `.cmd` and `.bat` first (#291).

mkdir -p _run_tmp/plo
dir=$(cd _run_tmp/plo && pwd)
printf '#!/bin/sh\necho "the POSIX script"\n' > "$dir/tool"
printf '@echo the batch file\r\n' > "$dir/tool.bat"
printf 'just some text\n' > "$dir/notes"
printf '@echo notes.cmd\r\n' > "$dir/notes.cmd"
chmod +x "$dir/tool"

echo "=== by path: the named script"
(cd "$dir" && command -v ./tool && ./tool)
"$dir/tool"

echo "=== by path: a plain file does not win over .cmd"
command -v "$dir/notes" | sed "s|^$dir/||"
"$dir/notes" | tr -d '\r'

echo "=== on PATH: .exe, .cmd and .bat first"
(
    PATH="$dir:$PATH"
    command -v tool | sed "s|^$dir/||"
    tool | tr -d '\r'
)

rm -rf _run_tmp/plo
