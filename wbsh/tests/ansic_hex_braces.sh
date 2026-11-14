#!/bin/bash
# $'\x{HHH}' (bash 5.3): any number of hex digits, the low byte kept, the
# closing brace optional; an empty value ends the string as a NUL does.
# A \x with no hex digit after it stays literal.

show() { printf '%s' "$1" | od -An -c | tr -s ' '; }
show $'ab\x{41}cd'
show $'ab\x{4}cd'
show $'ab\x{}cd'
show $'ab\x{c}de'
show $'\x{01234567X'
show $'\x{X'
show $'ab\x41cd'
show $'a\xZ'
show $'a\x'
