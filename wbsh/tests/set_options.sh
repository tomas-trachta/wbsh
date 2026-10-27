#!/bin/bash
# Probe: `set` option flags (bash 5.2 semantics).

strict() {
	set +o | grep -E " (errexit|nounset|pipefail)$"
}

echo "=== 1. strict-mode header: -o inside a cluster takes the next word"
(
	set -euo pipefail
	echo "args=$#"
	strict
	false | true
	echo "not reached"
)
echo "rc=$?"

echo "=== 2. each o in a cluster takes one word, in order"
set -oo pipefail errexit
strict
set +eo pipefail
set -eo pipefail -u x y
echo "n=$# 1=$1"
strict
set +euo pipefail --

echo "=== 3. +o in a cluster"
set -euo pipefail
set +euo pipefail
strict

echo "=== 4. set -o / set +o without a name list the options"
set -o
set +o
set -o pipefail -o | grep pipefail
set +o pipefail

echo "=== 5. invalid names and letters fail with status 2"
set -- keep
set -o bogus
echo "rc=$? n=$# 1=$1"
set -q
echo "rc=$? n=$# 1=$1"
set -e -Z x
echo "rc=$? n=$# 1=$1"
set +e

echo "=== 6. \$- shows the single-letter options"
echo "$-"
set -Ca
echo "$-"
set +Ca
set -fu
echo "$-"
set +fu

echo "=== 7. -C / noclobber"
rm -f _nc.txt
set -C
echo one > _nc.txt
echo "new file rc=$?"
echo two > _nc.txt
echo "overwrite rc=$?"
echo three >| _nc.txt
echo ">| rc=$?"
echo four >> _nc.txt
echo ">> rc=$?"
echo five &> _nc.txt
echo "&> rc=$?"
echo six > /dev/null
echo "/dev/null rc=$?"
cat _nc.txt
set +o noclobber
echo seven > _nc.txt
cat _nc.txt
rm -f _nc.txt

echo "=== 8. -a / allexport"
set -a
exported_plain=1
read exported_read <<< hi
declare exported_decl=2
arr_not_exported=(1 2)
set +a
not_exported=3
env | grep -E "^(exported_|arr_not_exported|not_exported)" | sort

echo "=== 9. -n / noexec stops running commands"
(
	echo before
	set -n
	echo skipped
	set +n
	echo still-skipped
)
echo "after rc=$?"

echo "=== 10. +B / braceexpand"
set +B
echo {a,b} x{1..3}
set -B
echo {a,b} x{1..3}

echo "=== 11. vi and emacs exclude each other"
set -o vi
set +o | grep -E " (emacs|vi)$"
set -o emacs
set +o | grep -E " (emacs|vi)$"
set +o emacs

echo "=== 12. -v / verbose echoes input lines as they are read"
printf 'echo sourced\n' > _verbose_src.sh
set -v
# a comment line
echo a; echo b
if true; then
	echo in-if
fi
cat <<E
heredoc body
E
eval 'echo from-eval'
x=$(echo not-echoed)
. ./_verbose_src.sh
set +v
echo quiet
rm -f _verbose_src.sh

echo "=== 13. set - turns verbose and xtrace off"
set -v
set - a
echo "n=$# 1=$1"

echo "=== 14. functions in a pipeline see the options"
show() {
	echo "in pipeline: $-"
}
set -u
show | cat
set +u
show | cat
