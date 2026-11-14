f1() { echo f1; }
function f2 { echo f2; }
function f3() { echo f3; }
f4() ( echo f4 subshell )
f5() if true; then echo f5; fi
f1; f2; f3; f4; f5
