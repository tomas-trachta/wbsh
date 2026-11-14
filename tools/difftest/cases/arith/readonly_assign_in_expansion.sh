readonly R=1
echo "before"
echo $(( R = 3 )); echo "same line"
echo "next line: $?"
