shopt -s extglob
shopt -p extglob nullglob
shopt -q nullglob; echo "nullglob rc=$?"
