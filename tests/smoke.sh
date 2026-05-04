#!/bin/bash
# Smoke test inputs — run wbsh on this file and inspect AST.

# 1. Quoting & expansions
echo "hello $USER" 'literal' \$escaped ${HOME}/bin $(date +%Y) $((1+2*3))

# 2. Pipeline + and/or
ls -la | grep foo | sort -u && echo done || echo fail

# 3. Redirections
cat <input.txt >out.txt 2>&1 3>>log.txt

# 4. Heredoc + heredoc-strip + here-string
cat <<EOF
hello
world
EOF
cat <<-MARK
	tabbed line
	another
	MARK
cat <<<"hi there"

# 5. If / elif / else
if [ -f foo ]; then
    echo yes
elif [ -d bar ]; then
    echo dir
else
    echo no
fi

# 6. For loops
for i in 1 2 3; do echo $i; done
for x do echo "$x"; done

# 7. While / until
while read line; do echo $line; done <input
until false; do break; done

# 8. Case
case "$x" in
    a|b) echo ab ;;
    c)   echo c  ;&
    *)   echo other ;;
esac

# 9. Function definitions
greet() { echo "hi $1"; }
function farewell { echo "bye $1"; }

# 10. Subshell, brace group, background
(cd /tmp; ls) &
{ echo a; echo b; } > combined.txt
