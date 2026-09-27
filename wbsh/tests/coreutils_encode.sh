#!/bin/bash
# Smoke test for the encoding/hash coreutils.

echo "=== md5sum / sha1sum / sha256sum / sha512sum"
printf 'abc' | md5sum
printf 'abc' | sha1sum
printf 'abc' | sha256sum
printf 'abc' | sha512sum

echo "=== base64 round-trip"
printf 'wbsh' | base64
printf 'd2JzaA==' | base64 -d
echo
printf 'Hello, World!\n' | base64 | base64 -d

echo "=== xxd"
printf 'ABC\n' | xxd
printf 'ABC\n' | xxd -p

echo "=== od"
printf 'AB\n' | od -c

echo "=== done"
