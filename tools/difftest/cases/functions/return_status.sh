f() { return 3; }
f; echo $?
g() { false; }
g; echo $?
h() { true; return; }
h; echo $?
k() { (exit 4); return; }
k; echo $?
