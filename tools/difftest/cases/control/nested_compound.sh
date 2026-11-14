x=0
if true; then
    for i in 1 2 3; do
        case $i in
            2) while (( x < 5 )); do (( x++ )); done ;;
        esac
    done
fi
echo $x
