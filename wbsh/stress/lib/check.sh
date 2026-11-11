# Assertion helpers for the stress cases that check properties (bugs/robust,
# security, ux). Source it, call the helpers, end with `finish`.
#
# Output is TAP-like and the driver (stress.ps1) parses it:
#     ok - <what>
#     not ok - [high] <what>          a failed check, with an optional severity
#     # review: <text>                a finding a human must judge; not a failure
#     # skip: <text>                  a check that could not run here
#     # <anything else>               a note, kept in the case log
#
# Severity is one of: critical, high, medium (the default), low.
#
# The environment the driver gives every case:
#     STRESS_WBSH   wbsh.exe under test (forward-slash Windows path)
#     STRESS_TMP    an empty work directory for this case (also the cwd)
#     STRESS_ROOT   wbsh/stress
#     STRESS_N      the scale factor of the active profile

STRESS_FAILS=0
STRESS_SEVERITY=medium

ok() {
    echo "ok - $*"
}

fail() {
    echo "not ok - [$STRESS_SEVERITY] $*"
    STRESS_FAILS=$((STRESS_FAILS + 1))
}

review() {
    echo "# review: $*"
}

skip() {
    echo "# skip: $*"
}

note() {
    echo "# $*"
}

# severity LEVEL -- the severity of the checks that follow.
severity() {
    STRESS_SEVERITY=$1
}

# check WHAT CMD [ARGS...] -- pass when CMD exits 0.
check() {
    local what
    what=$1
    shift
    if "$@"; then
        ok "$what"
    else
        fail "$what"
    fi
}

# expect_eq WHAT ACTUAL EXPECTED
expect_eq() {
    if [ "$2" = "$3" ]; then
        ok "$1"
    else
        fail "$1 (got '$2', want '$3')"
    fi
}

# expect_ne WHAT ACTUAL UNWANTED
expect_ne() {
    if [ "$2" != "$3" ]; then
        ok "$1"
    else
        fail "$1 (got unwanted '$2')"
    fi
}

# expect_match WHAT ACTUAL GLOB
expect_match() {
    case "$2" in
        $3) ok "$1" ;;
        *) fail "$1 (got '$2', want match for '$3')" ;;
    esac
}

# expect_no_match WHAT ACTUAL GLOB
expect_no_match() {
    case "$2" in
        $3) fail "$1 (got '$2', must not match '$3')" ;;
        *) ok "$1" ;;
    esac
}

# wbsh_run TEXT -- run TEXT as a script in a fresh wbsh with no stdin.
# Sets RUN_OUT (stdout), RUN_ERR (stderr) and RUN_RC. The text goes through
# a file, not -c: a Windows command line holds at most 32767 characters.
wbsh_run() {
    printf '%s\n' "$1" > "$STRESS_TMP/.run.sh"
    "$STRESS_WBSH" -r "$STRESS_TMP/.run.sh" > "$STRESS_TMP/.run.out" 2> "$STRESS_TMP/.run.err" < /dev/null
    RUN_RC=$?
    RUN_OUT=$(cat "$STRESS_TMP/.run.out")
    RUN_ERR=$(cat "$STRESS_TMP/.run.err")
}

finish() {
    if [ "$STRESS_FAILS" -gt 0 ]; then
        exit 1
    fi
    exit 0
}
