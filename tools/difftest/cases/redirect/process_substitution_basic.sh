cat <(echo from procsub)
diff_like() { while read -r a && read -r b <&3; do echo "$a=$b"; done 3< "$2" < "$1"; }
diff_like <(printf '1\n2\n') <(printf 'a\nb\n')
