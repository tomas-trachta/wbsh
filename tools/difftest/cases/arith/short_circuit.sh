x=0
echo $(( 0 && (x = 5) )) $x
echo $(( 1 || (x = 6) )) $x
echo $(( 1 && (x = 7) )) $x
