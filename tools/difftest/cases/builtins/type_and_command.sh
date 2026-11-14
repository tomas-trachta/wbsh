type echo
type -t cd
command -v echo
command -V echo 2>&1
f() { echo fn; }
command f 2>/dev/null || echo "command skips functions rc=$?"
