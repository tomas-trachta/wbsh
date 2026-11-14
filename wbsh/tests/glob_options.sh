#!/bin/bash
# Pathname expansion against bash: directory components, trailing slashes,
# whole-result sorting, failglob, GLOBIGNORE, globstar and nocasematch.

mkdir -p _run_tmp/globopt && cd _run_tmp/globopt
mkdir -p d1/sub d2 a a-b .hid t/a/b t/c
touch d1/f d2/f d2/g plain .dot d1/.x a/z a-b/z t/x.sh t/a/y.sh t/a/b/z.sh t/c/w.txt

echo "=== directory components"
echo "*/f:" */f
echo "d*/:" d*/
echo "*/*/:" */*/
echo "*/.x:" */.x
echo "d*/f/:" d*/f/
echo "nope*/f:" nope*/f
echo "./d*/:" ./d*/
echo "quoted slash:" d*'/'g
echo "whole result sorted:" */z

echo "=== failglob"
( shopt -s failglob; echo *.none; echo unreached ) 2>/dev/null
echo "subshell rc=$?"
msg=$( (shopt -s failglob; echo *.none) 2>&1 )
echo "message: no match${msg##*no match}"
( shopt -s failglob nullglob; for x in a* none*; do echo "loop $x"; done; echo unreached ) 2>/dev/null
echo "failglob beats nullglob rc=$?"
( shopt -s failglob; echo "matches still run:" d?/f )

echo "=== GLOBIGNORE"
GLOBIGNORE='*.dot:d1'
echo "ignore *.dot:d1:" *
GLOBIGNORE='*/f'
echo "ignore everything:" */f
unset GLOBIGNORE
echo "unset again:" *

echo "=== globstar"
shopt -s globstar
echo "t/**/*.sh:" t/**/*.sh
echo "t/**:" t/**
echo "t/**/:" t/**/
echo "**/z:" **/z
shopt -u globstar
echo "globstar off:" t/**/*.sh
shopt -s globstar

echo "=== nocasematch"
shopt -s nocasematch
case ABC in abc) echo "case: match" ;; *) echo "case: no match" ;; esac
case abc in A[B]*) echo "case bracket: match" ;; esac
[[ HELLO == h*o ]] && echo "[[ ]]: match"
shopt -u nocasematch
case ABC in abc) echo "case: match" ;; *) echo "case off: no match" ;; esac

cd ../.. && rm -rf _run_tmp/globopt
