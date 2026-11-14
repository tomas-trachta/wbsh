i=0
while (( i < 3 )); do echo "w$i"; (( i++ )); done
until (( i == 0 )); do echo "u$i"; (( i-- )); done
