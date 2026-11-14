mkdir x
cd x
cd - >/dev/null
echo "back: ${OLDPWD##*/}"
