for i in 1 2 3; do
    for j in a b c; do
        [ $j = b ] && continue
        [ $i = 2 ] && continue 2
        [ $i = 3 ] && break 2
        echo "$i$j"
    done
done
echo done
