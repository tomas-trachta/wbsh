{ echo a; echo b; } > grp
if true; then echo c; fi >> grp
for i in 1 2; do echo $i; done >> grp
cat grp
