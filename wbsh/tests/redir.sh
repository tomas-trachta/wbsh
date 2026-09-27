#!/bin/bash
# Smoke test for redirection forms.

D=_redir_tmp
rm -rf $D 2>/dev/null
mkdir -p $D

echo "=== > / >>"
echo "first" > $D/out.txt
echo "second" >> $D/out.txt
cat $D/out.txt

echo "=== <"
printf 'l1\nl2\nl3\n' > $D/in.txt
wc -l < $D/in.txt
cat < $D/in.txt

echo "=== 2> / 2>&1"
( cat _no_such_file 2>/dev/null; echo "after err" )
( cat _no_such_file > $D/combined.txt 2>&1; echo "captured" )
cat $D/combined.txt | head -1 | sed 's/[Nn]o such file or directory/<errmsg>/'

echo "=== &> (combined)"
( echo stdout; echo stderr 1>&2 ) > $D/all.txt 2>&1
sort $D/all.txt

echo "=== heredoc"
cat <<EOF
line a
line b with $((1+1))
EOF

cat <<'QUOTED'
no expansion: $((1+1))
QUOTED

cat <<-INDENT
	stripped
	tabs
	INDENT

echo "=== here-string"
cat <<<"hi from here-string"
grep -c o <<<"foo bar zoo"

echo "=== fd dup"
( exec 3>$D/fd.txt
  echo "via fd 3" >&3 )
cat $D/fd.txt

echo "=== pipe & redir interplay"
echo "abc" | tee $D/tee.txt > /dev/null
cat $D/tee.txt
{ echo a; echo b; echo c; } | wc -l

rm -rf $D 2>/dev/null
echo "=== done"
