readonly R=1
(( R = 12 ))
echo "arith command: $?"
let R=5
echo "let: $?"
(( R++ ))
echo "increment: $? R=$R"
