unset u; e=; s=set
echo "[${u-d1}] [${e-d2}] [${s-d3}]"
echo "[${u:-d1}] [${e:-d2}] [${s:-d3}]"
echo "[${u+a1}] [${e+a2}] [${s+a3}]"
echo "[${u:+a1}] [${e:+a2}] [${s:+a3}]"
