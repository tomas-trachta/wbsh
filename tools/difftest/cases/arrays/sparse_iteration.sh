a=([3]=c [1]=a [10]=j)
for i in "${!a[@]}"; do echo "$i=${a[$i]}"; done
