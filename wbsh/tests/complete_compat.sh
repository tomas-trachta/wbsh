#!/bin/bash
# What bash-completion and git's completion script lean on, with small
# functions of our own: COMP_WORDS cut at COMP_WORDBREAKS as bash cuts
# them (and the word a function is handed), COMP_TYPE and COMP_KEY,
# compopt from inside a function, the -o options, a -D loader returning
# 124 after defining the real spec, compgen -V, and complete -p for names
# that need quoting.
#
# `__lineedit` types its arguments (<TAB> among them) into a fresh line
# editor and prints the line as [before cursor][after cursor].

D=$(mktemp -d) || exit 1
cd "$D" || exit 1
mkdir dir1 dir2
touch file1 file2 'with space'

show() {
	printf 'CWORD=%s WORDS=' "$COMP_CWORD"
	printf '<%s>' "${COMP_WORDS[@]}"
	printf ' cur=<%s> prev=<%s> TYPE=%s KEY=%s\n' "$2" "$3" "$COMP_TYPE" "$COMP_KEY"
}

echo "=== COMP_WORDBREAKS"
printf '%q\n' "$COMP_WORDBREAKS"

echo "=== COMP_WORDS is cut at the break characters, runs of them kept whole"
complete -F show cmd
__lineedit 'cmd --opt=va' '<TAB>'
__lineedit 'cmd --opt=' '<TAB>'
__lineedit 'cmd a>>b c' '<TAB>'
__lineedit 'cmd "x=y" z' '<TAB>'
__lineedit 'cmd 2>&1 a' '<TAB>'

echo "=== without = in COMP_WORDBREAKS the word stays whole"
(
	COMP_WORDBREAKS=${COMP_WORDBREAKS//=/}
	__lineedit 'cmd --opt=va' '<TAB>'
	whole() { COMPREPLY=(--opt=value); }
	complete -F whole cmd
	__lineedit 'cmd --opt=va' '<TAB>'
)

echo "=== a reply goes in after the last break character"
value() { COMPREPLY=($(compgen -W 'value other' -- "$2")); }
complete -F value cmd
__lineedit 'cmd --opt=va' '<TAB>'

echo "=== COMP_TYPE: TAB-like, listing, menu, insert"
complete -F show cmd
bind 'set show-all-if-unmodified off'
__lineedit 'cmd x' '<TAB>'
__lineedit 'cmd x' '<M-?>'
__lineedit 'cmd x' '<M-*>'
bind '"\C-xm": menu-complete'
__lineedit 'cmd x' '<C-x>' 'm'
bind 'set show-all-if-unmodified on'

echo "=== compopt changes the running completion"
nospace() { compopt -o nospace; COMPREPLY=(word); }
complete -F nospace c1
__lineedit 'c1 w' '<TAB>'
names() { compopt -o filenames; COMPREPLY=('with space'); }
complete -F names c2
__lineedit 'c2 w' '<TAB>'
noquote() { compopt -o filenames -o noquote; COMPREPLY=('with space'); }
complete -F noquote c3
__lineedit 'c3 w' '<TAB>'
query() { compopt; COMPREPLY=(); }
complete -o nospace -F query c4
__lineedit 'c4 x' '<TAB>'

echo "=== -o options"
none() { COMPREPLY=(); }
complete -o default -F none o1
__lineedit 'o1 wi' '<TAB>'
complete -o dirnames -F none o2
__lineedit -o 'o2 d' '<TAB>' '<TAB>' | tail -2 | head -1
complete -o plusdirs -W 'dzz' o3
__lineedit -o 'o3 d' '<TAB>' '<TAB>' | tail -2 | head -1
complete -o bashdefault -F none o4
__lineedit 'o4 with' '<TAB>'
unsorted() { COMPREPLY=(zeta alpha mid); }
complete -o nosort -F unsorted o5
__lineedit -o 'o5 ' '<TAB>' '<TAB>' | tail -2 | head -1

echo "=== a -D loader that returns 124 has the real spec used"
loader() {
	local cmd=$1
	[ "$cmd" = lazy ] || return 1
	lazy_complete() { COMPREPLY=($(compgen -W 'loaded-word' -- "$2")); }
	complete -F lazy_complete lazy
	return 124
}
complete -D -F loader
__lineedit 'lazy lo' '<TAB>'
complete -p lazy
complete -r lazy
__lineedit 'other lo' '<TAB>'
complete -r -D

echo "=== compgen -V fills an array"
compgen -V found -W 'apple apricot banana' -- ap
declare -p found
compgen -V found -W 'apple' -- zz; echo "status $?"
declare -p found
complete -V x foo; echo "status $?"

echo "=== complete -p quotes names that need it"
complete -F show ''
complete -F show 'a b'
complete -p '' 'a b'
complete -p | grep -c show
complete -p | cat | grep -c "''"
complete -r '' 'a b'

echo "=== a reply word<TAB>description inserts the word"
described() {
	local word
	for word in $'--all\tshow everything' $'--brief\tshort form'; do
		[[ $word == "$2"* ]] && COMPREPLY+=("$word")
	done
}
complete -F described d1
__lineedit 'd1 --a' '<TAB>'
__lineedit -o 'd1 --' '<TAB>' '<TAB>' | tail -3 | head -2

cd / && rm -rf "$D"
