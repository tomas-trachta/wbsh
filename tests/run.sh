#!/bin/bash
# Smoke test for the executor.

echo "=== 1. Basic"
echo hello world
echo "with spaces" 'and quotes' $((2+3))

echo "=== 2. Pipelines"
echo -e "alpha\nbeta\ngamma\ndelta" | grep -v beta | sort -r

echo "=== 3. Redirection"
echo "to file" > /tmp/wbsh-out.txt 2>/dev/null || echo "to file" > wbsh-out.txt
cat wbsh-out.txt 2>/dev/null
rm wbsh-out.txt 2>/dev/null

echo "=== 4. Heredoc"
cat <<EOF
heredoc with $((1+1)) and end
EOF

echo "=== 5. Loops & control"
total=0
for i in 1 2 3 4 5; do
  total=$((total + i))
done
echo "sum 1..5 = $total"

i=0
while [ $i -lt 3 ]; do
  echo "while $i"
  i=$((i+1))
done

echo "=== 6. If / case"
n=42
if [ $n -gt 100 ]; then
  echo big
elif [ $n -gt 10 ]; then
  echo medium
else
  echo small
fi

case $n in
  4*) echo "starts with 4" ;;
  *)  echo other ;;
esac

echo "=== 7. Functions"
greet() { echo "hi $1"; }
greet alice
greet bob

fact() {
  if [ $1 -le 1 ]; then echo 1
  else local p=$(fact $(($1-1))); echo $(($1 * p))
  fi
}
echo "fact 6 = $(fact 6)"

echo "=== 8. Subshell isolation"
x=outer
( x=inner; echo "inside: $x" )
echo "outside: $x"

echo "=== 9. Brace group + redir"
{ echo a; echo b; echo c; } | grep b

echo "=== 10. && / ||"
true && echo and-ok
false || echo or-ok
true && false || echo "chain reached"

echo "=== done"
