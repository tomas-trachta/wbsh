shopt -s extglob
for w in foo.txt foo.c bar; do
    case $w in
        *.@(txt|md)) echo "$w doc" ;;
        !(*.*)) echo "$w noext" ;;
        *) echo "$w other" ;;
    esac
done
[[ abc == @(a|x)+(b)c ]] && echo match
