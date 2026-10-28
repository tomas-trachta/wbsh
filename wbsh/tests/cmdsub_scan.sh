#!/bin/bash
# $( ... ) bodies are lexed as commands, so `)` in case patterns, comments
# and heredocs does not end them; $(( falls back to a subshell when the
# text is not arithmetic.

echo "=== case patterns"
x=$(case a in a) echo A;; esac); echo $x
w=$(case b in (a) echo no;; (b) echo yes;; esac); echo $w
f() { echo $(case $1 in *) echo star;; esac); }; f k
cat <(case a in a) echo procsub;; esac)

echo "=== comments and heredocs"
y=$(echo hi # a comment )
); echo "[$y]"
z=$(cat <<EOT
a ) b
EOT
); echo "$z"

echo "=== quoting and nesting"
v="$(echo ")")"; echo "$v"
u=${q:-$(echo })}; echo "$u"
echo "$(echo "$(echo nested)")" $(echo $(echo deep))
echo $(echo a\)b)
echo `echo bq`
a=( $(echo 1 2) ); echo ${#a[@]}

echo "=== arithmetic or subshell"
x=$((echo a); echo b); echo $x
echo $((1+2)) $(( (1+2)*3 ))
echo $(( $(echo 2) * 3 ))
((echo sub); echo shell)
for ((i=0;i<2;i++)); do echo i$i; done

echo "=== heredoc bodies"
cat <<EOT
heredoc $(case a in a) echo X;; esac) $((2*3)) $((echo p); echo q)
EOT
