trap 'echo "exit trap rc=$?"' EXIT
echo body
exit 3
