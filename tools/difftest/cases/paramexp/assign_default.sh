unset u; e=
echo "${u=one} ${u}"
echo "[${e=two}] [${e}]"
echo "${e:=three} ${e}"
