# A <(...) path survives being read again by eval, can be opened again
# once drained (it then reads as empty), and stays open under exec.

eval cat <(echo test1)
eval "echo foo;cat" <(echo test2)
eval f=<(echo test4) "; cat \$f"

count_twice() {
    wc -l < "$1"
    wc -l < "$1"
}
count_twice <(printf 'a\nb\n') | tr -d ' '

exec 4< <(echo kept)
cat <(echo other) > /dev/null
read -r var <&4
echo "$var"
exec 4<&-
