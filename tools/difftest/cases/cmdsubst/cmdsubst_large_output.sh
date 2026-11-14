x=$(for i in {1..2000}; do echo "line $i"; done)
echo "${#x}"
