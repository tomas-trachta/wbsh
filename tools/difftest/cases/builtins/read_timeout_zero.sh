read -t 0 <<< "data"; echo "with data rc=$?"
read -t 0 < /dev/null; echo "devnull rc=$?"
