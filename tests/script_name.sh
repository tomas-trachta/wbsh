#!/bin/bash
# Verifies that `$0` reflects the path passed to wbsh (e.g.
# `wbsh -r script_name.sh`) rather than the literal string "wbsh".

echo "=== \$0 in main script"
echo "0=$0"

echo "=== \$0 inside a function"
who_am_i () {
    echo "fn: $0"
}
who_am_i

echo "=== \$0 inside a sourced script"
# Build the helper as a non-.sh file so run-all.sh's `for f in *.sh`
# doesn't try to run it as a top-level test.
HELPER=_run_tmp/script_name_helper
mkdir -p _run_tmp
cat > $HELPER <<'EOF'
echo "sourced: $0"
EOF
. $HELPER
rm -f $HELPER

echo "=== done"
