#!/bin/bash
# Probe: `set` and the positional parameters (bash 5.2 semantics).

xtrace_state() {
	case $- in
		*x*) echo on ;;
		*) echo off ;;
	esac
}

echo "=== 1. set -- with no operands clears the positionals"
set -- a b
set --
echo "n=$# all=[$*]"

echo "=== 2. set -- replaces them"
set -- x 'y z'
echo "n=$# 1=$1 2=$2"

echo "=== 3. options alone leave them alone"
set -- keep me
set -f
set +f
echo "n=$# all=[$*]"

echo "=== 4. options then -- clears"
set -- a b c
set -f --
echo "n=$#"
set +f

echo "=== 5. options then operands"
set -f p q
echo "n=$# all=[$*]"
set +f

echo "=== 6. - alone keeps them, - with operands replaces them"
set -- one two
set -
echo "n=$# 1=$1"
set - three
echo "n=$# 1=$1"

echo "=== 7. - turns xtrace off"
set -x
set - four five
echo "n=$# all=[$*] xtrace=$(xtrace_state)"

echo "=== 8. operands that look like options after --"
set -- -x +f --
echo "n=$# all=[$*] xtrace=$(xtrace_state)"

echo "=== 9. in a function the caller's positionals come back"
f() {
	set --
	echo "inside n=$#"
}
set -- outer args
f
echo "after n=$# all=[$*]"
