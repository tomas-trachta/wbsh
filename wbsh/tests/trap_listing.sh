#!/bin/bash
# trap: SIG-prefixed and numeric names, bash's listing format and order,
# trap -p SIG and resetting.

echo "=== setting and listing"
trap ': e' EXIT
trap ': r' RETURN
trap 'x' SIGINT
trap "it's" TERM
trap '' HUP
trap ': d' DEBUG
trap ': er' ERR
trap 'u' USR1 2
trap

echo "=== trap -p SIG"
trap -p INT
trap -p SIGTERM EXIT
trap -p sigusr1
trap -p FOO; echo "status $?"
trap 'x' FOO; echo "status $?"

echo "=== resetting"
trap INT
trap -- - TERM
trap 15 USR1
trap -p
trap - EXIT DEBUG ERR RETURN
trap -p

echo "=== trap -l"
trap -l | head -3
