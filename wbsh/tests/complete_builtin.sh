#!/bin/bash
# complete, compgen and compopt as in bash: every option is parsed (none
# is taken for a command name), complete -p prints specs complete can
# read back, and Tab completion honours what the spec says: actions,
# -G, -W, -F, -C, -X, -P, -S, -o options, and the -D, -E and -I specs.
#
# `__lineedit` types its arguments (<TAB> among them) into a fresh line
# editor and prints the line as [before cursor][after cursor].

echo "=== options are parsed, and complete -p reads back"
complete -A directory mycd
complete -X '!*.txt' -f vim
complete -o nospace -o filenames -W 'a b$x' -P pre -S suf -F fn -C 'cmd x' -G '*.c' \
    -A signal -A function -b -u foo
complete -D -F dflt
complete -E -W e
complete -I -c
complete -p
complete -p foo vim
complete -p -D -E -I
complete -p > specs.txt
complete -r
complete -p | wc -l
. ./specs.txt
complete -p | wc -l
rm -f specs.txt
complete -r

echo "=== errors"
complete -Z foo; echo "status $?"
complete -A nosuch foo; echo "status $?"
complete -o nosuch foo; echo "status $?"
complete -W; echo "status $?"
complete -F fn; echo "status $?"
complete -p nosuchcmd; echo "status $?"
complete -r nosuchcmd; echo "status $?"
compopt -o nospace; echo "status $?"

echo "=== compgen"
compgen -A signal SIGU
compgen -W 'alpha beta alps' al
compgen -W 'alpha beta alps' -X 'alp*' al; echo "status $?"
compgen -W 'alpha beta alps' -X '!alp*' a
compgen -W 'one two' -P '<' -S '>'
compgen -A setopt hi
compgen -k fi
x='1 2'
compgen -W '$x three'
mkdir -p cdir/sub cdir/other
touch cdir/file.txt cdir/file.c
compgen -d cdir/
compgen -f cdir/fi
compgen -G 'cdir/*.c'
compgen -o plusdirs -W 'cdir/x' cdir/

echo "=== compopt changes a spec's options"
complete -o filenames -W w foo
compopt -o nospace foo
compopt foo
compopt +o filenames foo
complete -p foo
complete -r foo

echo "=== Tab: word lists, prefixes, suffixes and filters"
complete -W 'start stop status' svc
__lineedit 'svc sto' '<TAB>'
__lineedit 'svc sta' '<TAB>'
complete -W 'key=' -o nospace cfg
__lineedit 'cfg k' '<TAB>'
complete -W 'one two' -P '<' -S '>' wrap
__lineedit 'wrap o' '<TAB>'

echo "=== Tab: directory and file actions, -X, -G"
complete -d mycd
__lineedit 'mycd cdir/s' '<TAB>'
complete -f -X '!*.txt' view
__lineedit 'view cdir/fi' '<TAB>'
complete -G 'cdir/*.c' cc1
__lineedit 'cc1 cdir/' '<TAB>'

echo "=== Tab: -F with compopt, -C, and COMPREPLY taken as it is"
_comp_opts() { compopt -o nospace; COMPREPLY=(chosen=); }
complete -F _comp_opts opts
__lineedit 'opts c' '<TAB>'
_comp_any() { COMPREPLY=(zz); }
complete -F _comp_any anyc
__lineedit 'anyc a' '<TAB>'
_comp_cmd() { compgen -W 'alpha beta' -- "$2"; }
complete -C _comp_cmd cmdc
__lineedit 'cmdc b' '<TAB>'

echo "=== Tab: a spec with no matches does not fall back to file names"
complete -W 'nothing' none
__lineedit 'none cdir/fi' '<TAB>'
complete -o default -W 'nothing' fallback
__lineedit 'fallback cdir/fi' '<TAB>'

echo "=== Tab: -D for commands without a spec, loading one on 124"
_comp_loader() { complete -W 'loaded' "$1"; return 124; }
complete -D -F _comp_loader
__lineedit 'newcmd l' '<TAB>'
complete -p newcmd
complete -r -D

echo "=== Tab: -E on an empty line, -I for the command word"
complete -E -W 'empty-line'
__lineedit '<TAB>'
complete -I -W 'initial-word'
__lineedit 'ini' '<TAB>'
complete -r -E -I

echo "=== a spec is found by the command's last path component"
__lineedit './svc sto' '<TAB>'

echo "=== a pipeline stage sees the specs"
complete -p svc | cat

rm -rf cdir
