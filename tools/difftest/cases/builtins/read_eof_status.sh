printf 'no newline' | { read -r x; echo "rc=$? x=[$x]"; }
read -r y < /dev/null; echo "rc=$? y=[$y]"
