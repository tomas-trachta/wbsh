#!/bin/bash
# Aggregating runner for the test suite.
#
# Usage:
#     ../x64/Release/wbsh.exe -r run-all.sh           # exit-status mode
#     WBSH_GOLDEN=1 ../x64/Release/wbsh.exe -r run-all.sh   # also diff vs expected/
#     WBSH_RECORD=1 ../x64/Release/wbsh.exe -r run-all.sh   # capture expected/
#
# In exit-status mode the runner asserts that each .sh script exits with the
# expected status (default 0). In golden mode it additionally diffs combined
# stdout+stderr against tests/expected/<name>.out, if that file exists. In
# record mode it (re)writes those expected files instead of diffing -- only
# run that when you trust the current output.

# tests that are exempt from a strict zero-exit assertion (none today; kept
# here so a future flaky test can be quarantined without losing coverage).
EXIT_NONZERO_OK=""

# tests whose output is non-deterministic across machines and so are NOT
# golden-checked even in WBSH_GOLDEN=1 mode (dates, host id, mounted-disk
# numbers, native-cmd output, etc.).
NO_GOLDEN="run.sh smoke.sh native.sh no_pathconv.sh paths.sh batch3.sh batch5.sh batch6.sh batch7.sh batch8.sh coreutils2.sh hello.sh"

GOLDEN_DIR=expected
TMP_DIR=_run_tmp
rm -rf $TMP_DIR 2>/dev/null
mkdir -p $TMP_DIR

if [ "${WBSH_RECORD:-0}" = "1" ]; then
    mkdir -p $GOLDEN_DIR
fi

# Discover wbsh.exe relative to the tests/ dir so the script works
# regardless of cwd. Prefer Release, fall back to Debug.
if [ -x ../x64/Release/wbsh.exe ]; then
    WBSH=../x64/Release/wbsh.exe
elif [ -x ../x64/Debug/wbsh.exe ]; then
    WBSH=../x64/Debug/wbsh.exe
else
    WBSH=wbsh
fi

is_in_list() {
    # NB: `local n=$1` in wbsh doesn't expand $1; assign separately.
    local needle haystack
    needle=$1
    haystack=$2
    case " $haystack " in
        *" $needle "*) return 0 ;;
        *) return 1 ;;
    esac
}

run_one() {
    local script=$1
    local out=$TMP_DIR/${script}.out
    local rc

    "$WBSH" -r "$script" > "$out" 2>&1
    rc=$?

    local expected_rc=0
    is_in_list "$script" "$EXIT_NONZERO_OK" && expected_rc=skip

    if [ "$expected_rc" = "skip" ]; then
        echo "  skip-rc  $script (exit=$rc)"
    elif [ "$rc" -eq "$expected_rc" ]; then
        printf "  ok       %s\n" "$script"
    else
        printf "  FAIL rc  %s (got %d, want %d)\n" "$script" "$rc" "$expected_rc"
        FAIL_COUNT=$((FAIL_COUNT + 1))
        return
    fi

    if [ "${WBSH_RECORD:-0}" = "1" ]; then
        if ! is_in_list "$script" "$NO_GOLDEN"; then
            cp "$out" "$GOLDEN_DIR/${script}.out"
            echo "           recorded $GOLDEN_DIR/${script}.out"
        fi
        return
    fi

    if [ "${WBSH_GOLDEN:-0}" = "1" ] && ! is_in_list "$script" "$NO_GOLDEN"; then
        if [ ! -f "$GOLDEN_DIR/${script}.out" ]; then
            printf "  no-golden %s (run with WBSH_RECORD=1 to capture)\n" "$script"
            return
        fi
        if diff -u "$GOLDEN_DIR/${script}.out" "$out" > "$TMP_DIR/${script}.diff" 2>&1; then
            printf "           golden ok\n"
        else
            printf "  FAIL out %s -- diff in %s/${script}.diff\n" "$script" "$TMP_DIR"
            head -20 "$TMP_DIR/${script}.diff"
            FAIL_COUNT=$((FAIL_COUNT + 1))
        fi
    fi
}

FAIL_COUNT=0
TOTAL=0
for f in *.sh; do
    [ "$f" = "run-all.sh" ] && continue
    TOTAL=$((TOTAL + 1))
    run_one "$f"
done

echo
echo "ran $TOTAL test scripts"
if [ "$FAIL_COUNT" -eq 0 ]; then
    echo "ALL PASS"
    rm -rf $TMP_DIR
    exit 0
else
    echo "$FAIL_COUNT FAILED"
    echo "(see $TMP_DIR/ for outputs and diffs)"
    exit 1
fi
