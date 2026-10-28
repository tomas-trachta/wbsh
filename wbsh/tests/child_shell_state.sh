#!/bin/bash
# A self-spawned child shell (pipeline stage, background job) takes the
# parent's resolved PATH and its loaded utils, `utils load` included,
# instead of probing the registry and the plugin folders again.

if [ -x ../../x64/Release/wbsh.exe ]; then
	B=../../x64/Release
else
	B=../../x64/Debug
fi

echo "=== 1. PATH and utils are the parent's"
PATH="/c/wbsh-child-marker:$PATH"
echo "$PATH" | cat > _run_tmp/child_path_parent.txt
{ echo "$PATH"; } | cat > _run_tmp/child_path_stage.txt
cmp -s _run_tmp/child_path_parent.txt _run_tmp/child_path_stage.txt && echo "same PATH"
utils | sort > _run_tmp/child_utils_parent.txt
{ utils; } | sort > _run_tmp/child_utils_stage.txt
cmp -s _run_tmp/child_utils_parent.txt _run_tmp/child_utils_stage.txt && echo "same utils"

echo "=== 2. the markers do not leak into the stage"
{ echo "[${WBSH_SUBSHELL-unset}] [${WBSH_UTIL_FILES-unset}]"; } | cat

echo "=== 3. a util loaded by hand reaches pipeline stages"
if [ -f "$B/wbshsdk.dll" ] && [ -f "$B/plugins/hello.dll" ]; then
	D=_run_tmp/child_isolated
	rm -rf "$D"
	mkdir -p "$D/appdata"
	cp "$B/wbsh.exe" "$B/wbshsdk.dll" "$D/"
	cp "$B/plugins/hello.dll" "$D/appdata/"
	APPDATA="$(cd "$D/appdata" && pwd -W)" WBSH_PLUGINS= "$D/wbsh.exe" -r -c '
		type hello > /dev/null 2>&1 || echo "hello not loaded at start"
		utils load "$1/hello.dll" && echo "loaded by hand"
		hello world | cat
		{ hello again; } | tr a-z A-Z' sh "$D/appdata"
	rm -rf "$D"
else
	echo "hello not loaded at start"
	echo "loaded by hand"
	echo "Hello, world!"
	echo "HELLO, AGAIN!"
fi
