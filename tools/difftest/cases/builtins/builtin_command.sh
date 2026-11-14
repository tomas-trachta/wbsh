echo() { printf 'override\n'; }
echo hi
builtin echo real
command echo also-real
unset -f echo
