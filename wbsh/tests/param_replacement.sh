#!/bin/bash
# The word parts of ${...}, as bash 5.2 has them:
# - in ${var/pattern/replacement}, an `&` stands for the matched text
#   (patsub_replacement, on by default); `\&` and a quoted `&` are a plain
#   `&`; a backslash in the literal text quotes the character after it;
#   what a variable puts there keeps its backslashes;
# - in an unquoted ${var:-word}, a backslash escapes the next character
#   (param_default_quoting.sh has the glob characters).

x=foo
echo "1 ${x/o/[&]}"
echo "2 ${x//o/<&>}"
echo "3 ${x/o/\&}"
echo "4 ${x/o/"&"}"
echo "5 ${x/o/'&'}"
echo "6 ${x/o/\z}"
echo "7 ${x/o/a\\b}"
amp='&x'
echo "8 ${x/o/$amp}"
echo "9 ${x/o/"$amp"}"
win='C:\dir\file'
echo "10 ${x/o/$win}"
s=a/b/c
echo "11 ${s//\//\\}"
url='a=1&b=2'
echo "12 ${url//&/&amp;}"
shopt -u patsub_replacement
echo "13 ${x/o/[&]}"
shopt -s patsub_replacement

u=
echo 14 ${u:-\z}
echo "15 ${u:-\z}"
echo 16 ${u:-a\ b}
echo 17 ${u:-\$HOME}
echo 18 ${x:+\y}
