for w in apple banana cherry 42 ''; do
    case $w in
        a*) echo "$w: a-word" ;;
        b*|c*) echo "$w: b-or-c" ;;
        [0-9]*) echo "$w: number" ;;
        '') echo "empty" ;;
    esac
done
