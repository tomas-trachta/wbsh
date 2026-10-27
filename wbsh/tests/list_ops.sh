#!/bin/bash
# Probe: operators on `$@` / `$*` / `${a[@]}` / `${a[*]}` apply to each
# element, and the `@` forms keep the elements as separate fields.
# Expected output matches bash 5.2.

show() {
    printf 'n=%d' $#
    for arg; do printf ' [%s]' "$arg"; done
    echo
}

echo "=== 1. per-element operators on an array"
a=(ab cd)
show ${a[@]^} ${a[@]#?}
show "${a[@]%?}" "${a[@]^^}" "${a[@],}"
show "${a[@]/b/B}" "${a[@]//?/-}" "${a[@]/#a/A}" "${a[@]/%d/D}"
show "${a[*]^}"
show ${a[*]^}
x=hello
show "${x[@]^}" "${x[*]^^}"

echo "=== 2. per-element operators on the positionals"
ops() {
    show "${@#p}"
    show "${*#p}"
    show "${@/ /_}"
    show "${@^^}"
    show "${@%?}"
    show ${@#p}
}
ops "p 1" "q 2"

echo "=== 3. positional slices"
slices() {
    show "${@:1}"
    show "${@:2}"
    show "${@: -1}"
    show "${@: -2}"
    show "${@:1:1}"
    show "${@:5}"
    show "${@:1:0}"
    show "${*:1}"
    show ${@:1}
    show pre"${@:2}"post
    joined="${@:2}"
    show "$joined"
    [ "${@:0:1}" = "$0" ] && echo "  \${@:0:1} is \$0"
}
slices "p 1" "q 2" r

echo "=== 4. array slices"
c=(x "y z" w)
show "${c[@]:1}"
show "${c[@]: -1}"
show "${c[@]:1:1}"
show "${c[*]:1}"
d=([2]=a [5]=b [7]=c)
show "${d[@]:3}"
show "${d[@]:1:1}"
show "${d[@]: -1}"
show "${d[@]: -3}"
show "${d[@]: -9}"
show "${x[@]:1}" "${x[@]: -2}"

echo "=== 5. empty lists give zero fields"
empty() {
    show "${@:1}"
    show "${@#x}"
    show x"${@#x}"
    show "${*#x}"
    show "${@:+x}"
    show "${@+x}"
}
empty
e=()
show "${e[@]#x}" "${e[@]:0}" "${e[@]^}"

echo "=== 6. default operators"
defaults() {
    show "${@:-d}"
    show "${@-d}"
    show "${@:+x y}"
    show ${@:+x y}
}
echo "-- none"
defaults
echo "-- one empty"
defaults ""
echo "-- two empty"
defaults "" ""
echo "-- two"
defaults "a b" c
show "${e[@]:-d}"

echo "=== 7. IFS"
c=(x "y z" w)
IFS=:
show "${c[*]#x}"
show ${c[@]#x}
unset IFS

echo "=== 8. heredoc"
here() {
    cat <<EOT
slice: ${@:2}
strip: ${@#?}
EOT
}
here ab cd ef
