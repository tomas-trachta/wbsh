#!/bin/bash
# Prompt strings as bash builds them: the backslash escapes, then (shopt
# promptvars) parameter, command and arithmetic expansion, and
# PROMPT_COMMAND before each primary prompt. `${x@P}` decodes a value the
# way PS1 is decoded.

echo "=== counters and fixed text"
x='[\j] [\!] [\#] [\s]'
echo "${x@P}"
x='\l'
[[ ${x@P} == console || ${x@P} == tty ]] && echo "\\l ok"

echo "=== \\nnn is an octal character code; \\\\ a backslash; unknown escapes stay"
x='\101\102\103 \\ \q \0411'
echo "${x@P}"

echo "=== the date and time escapes"
x='\d|\t|\T|\@|\A'
[[ ${x@P} =~ ^[A-Z][a-z]{2}\ [A-Z][a-z]{2}\ [0-9]{2}\|[0-9]{2}:[0-9]{2}:[0-9]{2}\|[0-9]{2}:[0-9]{2}:[0-9]{2}\|[0-9]{2}:[0-9]{2}\ (AM|PM)\|[0-9]{2}:[0-9]{2}$ ]] && echo "times ok"
x='\D{%Y}'
[[ ${x@P} == "$(date +%Y)" ]] && echo "\\D{%Y} ok"
x='[\D{}]'
[[ ${x@P} =~ ^\[[0-9]{1,2}:[0-9]{2}:[0-9]{2}\]$ ]] && echo "\\D{} is the locale time"
x='\D'
echo "${x@P}"

echo "=== the version escapes"
x='\v|\V'
[[ ${x@P} =~ ^[0-9]+\.[0-9]+\|[0-9]+\.[0-9]+\.[0-9]+$ ]] && echo "versions ok"

echo "=== \\$ is \$, or # in an elevated shell"
x='\$'
[[ ${x@P} == '$' || ${x@P} == '#' ]] && echo "\\$ ok"

echo "=== promptvars: parameter, command and arithmetic expansion after the escapes"
name=world
x='hello $name $(echo cmd) $((6*7)) `echo back`'
echo "${x@P}"
x='\u$name'
[[ ${x@P} == "${USER:-$USERNAME}world" ]] && echo "\\u then \$name ok"

echo "=== text from an escape is not expanded again"
T=$(mktemp -d) || exit 1
mkdir -p "$T/prompt_\$HOME_dir" && cd "$T/prompt_\$HOME_dir" || exit 1
x='\W'
echo "${x@P}"
cd - > /dev/null
rm -rf "$T"

echo "=== a backslash quotes \$ \` \" and itself in the expansion"
x='a\$b \`c\` \"d\" e\\f'
echo "${x@P}"

echo "=== with promptvars off only the escapes are decoded"
shopt -u promptvars
x='$name \$ \101'
echo "${x@P}"
shopt promptvars
shopt -s promptvars

echo "=== an interactive shell: PS1 expansion, \\#, \\!, PROMPT_COMMAND"
WBSH=../../x64/Release/wbsh.exe
[ -x "$WBSH" ] || WBSH=../../x64/Debug/wbsh.exe
D=$(mktemp -d) || exit 1
session() {
    (
        env -u WBSH_INIT_COMMAND -u PROMPT_COMMAND HOME="$D" HISTFILE="$D/hist" PS2='> ' \
            "$@" "$WBSH" -i 2>&1
        echo "(exit $?)"
    ) | sed -n '/^\[/,$p'
}

session PS1='[\#:\!:$(echo $((1+1)))]\$ ' <<'EOF'
echo one
echo two
EOF

echo "--- \$? is not changed by the prompt's command substitution"
session PS1='[$(false)]\$ ' <<'EOF'
false
echo "status=$?"
true
echo "status=$?"
EOF

echo "--- PROMPT_COMMAND runs before each primary prompt and keeps \$?"
session PS1='[p]\$ ' <<'EOF'
PROMPT_COMMAND='echo "<pc>"'
false
echo "status=$?"
EOF

echo "--- an array PROMPT_COMMAND runs every element"
session PS1='[p]\$ ' <<'EOF'
PROMPT_COMMAND=('echo first' 'echo second')
echo body
EOF

echo "--- PROMPT_COMMAND is not run before a continuation prompt"
session PS1='[p]\$ ' <<'EOF'
PROMPT_COMMAND='echo "<pc>"'
if true
then echo inside
fi
EOF

echo "--- exit in PROMPT_COMMAND ends the shell"
session PS1='[p]\$ ' <<'EOF'
PROMPT_COMMAND='exit 7'
echo never
EOF
rm -rf "$D"
