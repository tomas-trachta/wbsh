#!/bin/bash
# Probe: heredoc edge cases.
NAME=world
NUM=42

echo "=== 1. unquoted delim - parameter / cmdsubst / arith expand"
cat <<EOF
hello $NAME
result: $(echo computed)
math: $((1 + 2 + 3))
literal-tilde: ~
EOF

echo "=== 2. single-quoted delim - body is verbatim"
cat <<'EOF'
hello $NAME
result: $(echo computed)
math: $((1 + 2 + 3))
EOF

echo "=== 3. double-quoted delim - same as single (no expansion)"
cat <<"EOF"
hello $NAME
EOF

echo "=== 4. <<- strips ONLY leading tabs, not spaces"
cat <<-EOF
	tab-indented (stripped)
		double-tab (stripped)
    space-indented (kept)
	mixed	tab	in	the	middle
	EOF

echo "=== 5. heredoc inside a function"
greet() {
    cat <<EOF
greet for $1, count=$NUM
EOF
}
greet alice

echo "=== 6. heredoc piped into another command"
cat <<EOF | tr a-z A-Z
shouting at $NAME
EOF

echo "=== 7. heredoc with backslash escapes"
cat <<EOF
backslash-escape-of-dollar: \$NAME
backslash-escape-of-backslash: \\
backslash-other: \x
EOF

echo "=== 8. heredoc redirected to a file then back"
mkdir -p _run_tmp
cat > _run_tmp/h.txt <<EOF
saved $NAME
EOF
cat _run_tmp/h.txt
rm -rf _run_tmp

echo "=== done"
