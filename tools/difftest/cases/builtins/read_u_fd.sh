exec 3<<< "via fd"
read -r -u 3 line
echo "$line"
