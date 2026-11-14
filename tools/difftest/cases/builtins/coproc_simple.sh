coproc CO { read -r line; echo "got $line"; }
echo hello >&"${CO[1]}"
read -r reply <&"${CO[0]}"
echo "$reply"
wait
