#!/bin/bash
# Tab completion looks at the command the cursor is in, not the first
# word of the line: COMP_LINE, COMP_POINT, COMP_WORDS and COMP_CWORD
# cover that command only, and a -F function's $3 is the word before
# the one being completed (the command name for its first argument).

show() {
	echo "LINE=[$COMP_LINE] POINT=$COMP_POINT CWORD=$COMP_CWORD WORDS=[${COMP_WORDS[*]}] ARGS=[$1|$2|$3]"
}
complete -F show foo
complete -W "main other" sw

echo "=== the spec of the command after a separator is used"
__lineedit 'make && sw ma' '<TAB>'
__lineedit 'make; sw ma' '<TAB>'
__lineedit 'cat x | sw ma' '<TAB>'
__lineedit '(sw ma' '<TAB>'
__lineedit '{ sw ma' '<TAB>'
__lineedit 'true || sw o' '<TAB>'

echo "=== the completion function sees only the current command"
__lineedit 'make && foo ab' '<TAB>'
__lineedit 'foo' ' ' '<TAB>'
__lineedit 'echo hi; foo a b' '<TAB>'
__lineedit 'make && foo ab cd; ls' '<LEFT>' '<LEFT>' '<LEFT>' '<LEFT>' '<LEFT>' '<LEFT>' '<LEFT>' '<TAB>'
__lineedit 'foo ab cd; ls' '<LEFT>' '<LEFT>' '<LEFT>' '<LEFT>' '<LEFT>' '<LEFT>' '<LEFT>' '<LEFT>' '<TAB>'

echo "=== quotes and expansions stay one word and hide separators"
__lineedit 'foo "a b" c' '<TAB>'
__lineedit 'foo "x;y" c' '<TAB>'
__lineedit "foo 'a|b' c" '<TAB>'
__lineedit 'foo $(echo x; echo y) ' '<TAB>'
__lineedit 'foo ${x} z' '<TAB>'

echo "=== a redirection's & does not start a command; assignments are skipped"
__lineedit 'foo 2>&1 a' '<TAB>'
__lineedit 'X=1 Y[2]=3 foo a' '<TAB>'
__lineedit 'ls; X=1 foo a' '<TAB>'

echo "=== the first word of a command still completes as a command"
__lineedit 'true && __lineed' '<TAB>'
__lineedit 'X=1 __lineed' '<TAB>'
