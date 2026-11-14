cat <<< "here string"
x="a b"
cat <<< $x
cat <<< "$(echo multi; echo line)"
