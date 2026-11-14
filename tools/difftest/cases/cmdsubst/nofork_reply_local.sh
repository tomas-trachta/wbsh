# requires: bash>=5.3
REPLY=outer
r=${| REPLY=inner; }
echo "$r $REPLY"
