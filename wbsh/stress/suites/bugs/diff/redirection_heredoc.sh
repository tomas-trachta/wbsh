#!/bin/bash
# Redirections, here-documents and descriptor juggling.
echo one > f; echo two >> f; cat f
{ echo out; echo err >&2; } 2>&1 | sort
{ echo out; echo err >&2; } 2>/dev/null
{ echo out; echo err >&2; } > /dev/null 2>&1; echo silent
exec 3> g; echo via3 >&3; exec 3>&-; cat g
exec 4< f; read -r l <&4; echo "read $l"; exec 4<&-
cat <<EOF
plain $HOME_UNSET_X $(echo sub) $((1+1))
	tab kept
EOF
cat <<'EOF'
literal $x $(no)
EOF
cat <<-EOF
	tab stripped
	EOF
cat <<< "here string"
x=$(cat <<EOF
inside subst
EOF
)
echo "$x"
echo hello > f 2>&1; cat < f
printf '%s\n' c a b | sort > s; cat s
: > empty; wc -c < empty
echo a >| f; cat f
cat f nofile 2>/dev/null; echo "rc $?"
