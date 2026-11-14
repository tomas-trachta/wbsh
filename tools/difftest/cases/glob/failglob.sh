mkdir g && cd g && touch x
shopt -s failglob
echo *.none
echo "rc=$?"
echo still running
