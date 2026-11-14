# requires: bash>=5.0
echo $(( LINENO ))
(( SECONDS >= 0 )) && echo seconds
(( EPOCHSECONDS > 1000000000 )) && echo epoch
RANDOM=7; a=$RANDOM; RANDOM=7; (( RANDOM == a )) && echo random
