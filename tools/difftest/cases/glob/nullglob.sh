mkdir g && cd g && touch x
shopt -s nullglob
set -- *.none
echo "$#"
echo *.none x*
