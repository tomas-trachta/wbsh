f() { trap 'echo func exit trap' EXIT; echo in f; }
f
echo after f
