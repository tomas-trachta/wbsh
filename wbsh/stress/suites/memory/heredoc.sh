#!/bin/bash
# Here-documents and here-strings.
# @stress n=1000
for ((i = 0; i < STRESS_ITERS; i++)); do
    cat > /dev/null <<END
line $i
another line with $HOME
END
    read -r x <<< "here string $i"
done
echo ok
