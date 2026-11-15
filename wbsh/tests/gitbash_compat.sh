#!/bin/bash
# The Git Bash helpers scripts call (cygpath, start, winpty), cat's
# -A / -v / -E / -T / -s family, and the time keyword's report.

if [ -x ../../x64/Release/wbsh.exe ]; then
	W=../../x64/Release/wbsh.exe
else
	W=../../x64/Debug/wbsh.exe
fi

W=$PWD/$W
T=_gitbash_compat_tmp
rm -rf $T
mkdir -p $T
cd $T

c() {
	printf '%-28s -> ' "$*"
	cygpath "$@"
	echo "[$?]"
}

echo "=== cygpath to unix"
c -u 'C:\Users\x'
c -u 'C:\Users\x\'
c -u C:/Users/x
c 'c:\x'
c --unix 'D:\a b\c'
c -u C:
c -u 'C:\'
c -u 'C:foo'
c -u 'foo\bar'
c -u foo/bar
c -u /c/x//y
c -u 'C:\x\..\y'
c -u 'C:\x\.\y\'
c -u '\\server\share\x'
c -u '~'

echo "=== cygpath to windows and mixed"
c -w /c/Users/x
c -w /c/Users/x/
c -m /c/Users/x
c -w /c
c -w /c/
c -w C:/x
c -w 'C:\Users\..'
c -w /c/x//y
c -m 'C:/Users//x/'
c -m 'foo\bar'
c -w foo/bar
c -w ./foo
c -w a/./b
c -w '\\server\share\x'
c -m //server/share/x
c -w /dev/null
c --windows /d/a /e/b
c -t mixed /c/x
c -t windows /c/x
c --type unix 'C:\x'

echo "=== cygpath path lists"
c -w -p /c/a:/d/b
c -m -p /c/a:/d/b
c -w -p /c/a::/d/b
c -w -p a:b
c -u -p 'C:\a;D:\b'
c -u -p 'C:\a;;D:\b'
c -u -p /c/a:/c/b
c -m -p 'C:\x:/c/y'
c --path --mixed '/c/a;/d/b'

echo "=== cygpath against the shell's own view"
[ "$(cygpath -u -a foo)" = "$(pwd)/foo" ] && echo "-u -a foo is \$PWD/foo"
[ "$(cygpath -u -a foo/)" = "$(pwd)/foo/" ] && echo "-u -a keeps a trailing slash"
[ "$(cygpath -w -a foo)" = "$(cygpath -w "$(pwd)")\\foo" ] && echo "-w -a foo is \$PWD\\foo"
[ "$(cygpath -m -a ../x)" = "$(cygpath -m "$(dirname "$(pwd)")")/x" ] && echo "-m -a ../x"
[ "$(cygpath -w ../x)" = "$(cygpath -w -a ../x)" ] && echo "-w ../x resolves"
[ "$(cygpath -u -a /c/x/./y)" = /c/x/./y ] && echo "-u -a leaves a POSIX name alone"
[ "$(cygpath -u "$(cygpath -w /c/Windows/System32)")" = /c/Windows/System32 ] && echo "round trip"
case "$(cygpath -w /tmp/x)" in
	[A-Z]:\\*\\x) echo "/tmp maps to a drive path" ;;
	*)            echo "/tmp UNEXPECTED: $(cygpath -w /tmp/x)" ;;
esac
[ "$(cygpath -u "$(cygpath -w /tmp)")" = "$(cygpath -u "$(cygpath -m /tmp)")" ] && echo "-w and -m agree"
mkdir -p 'long dir name here'
short=$(cygpath -m -s 'long dir name here')
[ "$(cygpath -m -l "$short")" = 'long dir name here' ] && echo "-s then -l round trips"
[ "$(cygpath -m -l -a "$short")" = "$(cygpath -m -a 'long dir name here')" ] && echo "-l -a gives the long absolute name"

echo "=== cygpath short and long names of missing paths"
c -w -s 'C:\definitely\not\here'
c -m -l 'C:\nonexist\PROGRA~1'

echo "=== cygpath -f and -0"
printf 'C:\\a\n/c/b\nfoo\\bar\n' > names
c -u -f names
c -m -f names
printf '/c/x\n' | cygpath -w -f -
printf 'C:\\a\n\nC:\\b\n' > gap
c -u -f gap
c -u -f missing_file
cygpath -0 -u 'C:\a' 'C:\b' | od -An -c | tr -s ' '
cygpath --null -w /c/a | od -An -c | tr -s ' '

echo "=== cygpath special folders"
case "$(cygpath -W)" in /[a-z]/[Ww][Ii][Nn][Dd][Oo][Ww][Ss]) echo "-W ok" ;; *) echo "-W $(cygpath -W)" ;; esac
case "$(cygpath -w -S)" in [A-Z]:\\*\\[Ss]ystem32) echo "-S ok" ;; *) echo "-S $(cygpath -w -S)" ;; esac
case "$(cygpath -m -H)" in [A-Z]:/*) echo "-H ok" ;; *) echo "-H $(cygpath -m -H)" ;; esac
case "$(cygpath -D)" in /[a-z]/*) echo "-D ok" ;; *) echo "-D $(cygpath -D)" ;; esac
case "$(cygpath -O)" in /[a-z]/*) echo "-O ok" ;; *) echo "-O $(cygpath -O)" ;; esac
case "$(cygpath -P)" in /[a-z]/*/Programs) echo "-P ok" ;; *) echo "-P $(cygpath -P)" ;; esac
case "$(cygpath -A -D)" in /[a-z]/*) echo "-A -D ok" ;; *) echo "-A -D $(cygpath -A -D)" ;; esac
[ "$(cygpath -F 36)" = "$(cygpath -W)" ] && echo "-F 36 is the Windows directory"
[ "$(cygpath --folder 0x25)" = "$(cygpath -S)" ] && echo "--folder 0x25 is the system directory"
c -F 999
c -F abc

echo "=== cygpath usage"
cygpath; echo "[$?]"
c -u ''
c -x
c --bogus
c -t bogus x
cygpath -F; echo "[$?]"
cygpath --help | head -1
cygpath -h | grep -c -- '--mixed'
cygpath --version | sed 's/[0-9][0-9]*/N/g'

echo "=== man pages"
whatis cygpath start winpty

echo "=== start"
start; echo "[$?]"
start --help | head -2
start --version | sed 's/[0-9][0-9]*/N/g'
start definitely_missing_wbsh_file.txt; echo "[$?]"
start /wait '' definitely_missing_wbsh_file.txt; echo "[$?]"
type start | head -1

echo "=== winpty"
winpty echo hi; echo "[$?]"
winpty -Xallow-non-tty -Xplain --mouse echo plain; echo "[$?]"
f() { echo "f got $*"; return 3; }
winpty f a 'b c'; echo "[$?]"
winpty -- printf '%s\n' dashdash
winpty "$W" -c 'echo child; exit 4'; echo "[$?]"
winpty false; echo "[$?]"
winpty definitely_missing_wbsh_cmd; echo "[$?]"
winpty; echo "[$?]"
winpty --version | sed 's/[0-9][0-9]*/N/g'
winpty -h | head -1

echo "=== cat -v family"
printf 'a\r\nb\tc\001\177\200\237\240\377\351\n\n\n\nend' > ctl
cat -A ctl; echo
cat -v ctl; echo
cat -e ctl; echo
cat -t ctl; echo
cat -E ctl | od -An -c | tr -s ' '
cat -T ctl | od -An -c | tr -s ' '
cat --show-all ctl; echo
cat --show-nonprinting --show-ends --show-tabs ctl; echo
cat -vET - < ctl; echo

echo "=== cat numbering and squeezing"
printf 'x\n\n\n\ny\n\n' > blanks
cat -s blanks
echo "--"
cat -sn blanks
echo "--"
cat -bn blanks
echo "--"
cat -bs blanks
echo "--"
cat --squeeze-blank --number-nonblank blanks
echo "--"
cat -n blanks ctl | head -7 | tr -d '\r'
echo "--"
cat -u blanks | wc -l
cat -nq blanks 2>&1; echo "[$?]"

echo "=== time report"
digits() {
	sed 's/[0-9]/N/g'
}
{ time true; } 2>&1 | digits | od -An -c | tr -s ' '
{ time -p true; } 2>&1 | digits
{ time -p true | cat; } 2>&1 | digits
TIMEFORMAT='R=%R U=%U S=%S P=%P pct=%% one=%1R none=%0R long=%lR two-long=%2lU'
{ time true; } 2>&1 | digits
TIMEFORMAT=''
{ time true; } 2>&1 | wc -c
{ time -p true; } 2>&1 | digits
unset TIMEFORMAT
{ time ! false; } 2>&1 | digits | tr '\n\t' '|_'
echo
{ time sleep 0.2; } 2>&1 | digits | tr '\n\t' '|_'
echo
TIMEFORMAT=%3R
real=$({ time sleep 0.2; } 2>&1)
if [ $(( 10#${real/./} >= 190 )) = 1 ]; then echo "real covers the sleep"; else echo "real=$real"; fi
TIMEFORMAT='%3U %3S'
child=$({ time "$W" -c 'i=0; while [ $i -lt 100000 ]; do i=$((i+1)); done'; } 2>&1)
set -- $child
if [ $(( 10#${1/./} + 10#${2/./} > 0 )) = 1 ]; then echo "user+sys counts the child"; else echo "child=$child"; fi
own=$({ time for ((i = 0; i < 30000; i++)); do :; done; } 2>&1)
set -- $own
[ $(( 10#${1/./} + 10#${2/./} > 0 )) = 1 ] && echo "user+sys counts builtin work"
unset TIMEFORMAT
{ time false; } 2>/dev/null; echo "time false: $?"

cd ..
rm -rf $T
