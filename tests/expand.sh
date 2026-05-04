#!/bin/bash
# Smoke test for the expander stage.

# 1. Basic and quoting
echo plain "double $HOME" 'single $HOME' \$lit

# 2. Parameter operators
V=aXbXcXd
echo "${V/X/-}" "${V//X/-}" "${V#a}" "${V##*X}" "${V%d}" "${V%%X*}" "${V:2}" "${V:2:3}" "${#V}"

# 3. Default-value forms
echo "${MISSING:-fallback}" "${MISSING:+set}" "${V:-fallback}" "${V:+set}"

# 4. Arithmetic
echo $((1+2*3)) $((2**8)) $((0xff)) $((010)) $(((5>3)?100:200)) $((10%3))
echo $((x = 5)) $((x += 3)) $((x++)) $((++x))

# 5. ANSI-C
echo $'tab\there\nnewline\\backslash\x41\101'

# 6. Tilde
echo ~ ~/bin

# 7. Heredoc body
cat <<EOF
hello $HOME
EOF
cat <<'QUOTED'
no expansion: $HOME
QUOTED

# 8. Globbing
echo *.md
echo no-such-pattern*

# 9. Empty-quoted survives
echo before "" after
echo before $undef after
