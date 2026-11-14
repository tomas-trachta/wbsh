#!/bin/bash
# MSYS2-style conversion of the arguments a native program gets, as Git Bash
# does it. cmd's echo prints each argument the way cmd received it.

show() {
    printf '%-22s -> ' "$1"
    cmd //c echo "$1" | tr -d '\r'
}

echo "=== plain paths"
for a in /c/Users /c/ /c/x//y '/c/x\y' /c:/x /dev/null; do show "$a"; done

echo "=== two-letter switches stay; // unescapes"
for a in /c /F //c //F //PID // /// ///c; do show "$a"; done

echo "=== UNC paths stay"
for a in //server/share //server/share/x //c/x //c/ //:x; do show "$a"; done

echo "=== options and assignments"
for a in --opt=/c/x -I/c/x -L/c/x -o/c/x -D/c/x -I=/c/x -I//c -DFOO=/c/x \
    VAR=/c/x a=b=/c/x /c/x=y -Wl,/c/x -Wl,-rpath,/c/x +/c/x @/c/x =/c/x; do
    show "$a"
done

echo "=== option forms that stay"
for a in -ab/c/x --/c/x -/c/x -1/c/x foo,/c/x x=y:/c/z a=b:/c/x; do show "$a"; done

echo "=== path lists"
for a in /c/x:/d/y -I/c/x:/d/y --opt=/c/x:/d/y x=/c/a:/c/b /c/x:. .:/c/x; do show "$a"; done

echo "=== lists with an unconvertible entry stay"
for a in /c/x:/usr /c/x:./y foo:/c/x :/c/x; do show "$a"; done

echo "=== not paths"
for a in http://example.com/a https://x/y:/z s/a/b/ a:b file.cpp:12 /c/x:12 /c/x: \
    a::b /c/x::y /c/x:http://a rel/x ./rel/path ../x /../x C:/x 'C:\x' '~/x' /c/~x; do
    show "$a"
done

echo "=== glob, quote and list characters stop conversion"
for a in '/c/x*' '/c/x?' '/c/[ab]' "/c/x'y" '/c/x`y' '/c/x;y' '/c/x;/c/y' /c/x@@y; do
    show "$a"
done

echo "=== paths no mount covers stay"
for a in / /usr/bin /ab /dev/stdin /dev/tty; do show "$a"; done

echo "=== MSYS2_ARG_CONV_EXCL"
MSYS2_ARG_CONV_EXCL='--foo=;/c/U' cmd //c echo --foo=/c/x /c/Users /c/x --bar=/c/x | tr -d '\r'
MSYS2_ARG_CONV_EXCL='/c/x;' cmd //c echo /c/x /c/y | tr -d '\r'
MSYS2_ARG_CONV_EXCL='*' cmd /c echo /c/x //y | tr -d '\r'
export MSYS2_ARG_CONV_EXCL='/c/y'
cmd //c echo /c/x /c/y | tr -d '\r'
unset MSYS2_ARG_CONV_EXCL
cmd //c echo /c/x /c/y | tr -d '\r'

echo "=== MSYS_NO_PATHCONV keeps // too"
MSYS_NO_PATHCONV=1 cmd /c echo //c /c/y /dev/null | tr -d '\r'

echo "=== done"
