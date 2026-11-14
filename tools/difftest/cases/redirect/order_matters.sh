f() { echo out; echo err >&2; }
f 2>&1 >/dev/null | tr a-z A-Z
