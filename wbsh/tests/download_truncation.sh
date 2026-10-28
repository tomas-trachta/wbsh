#!/bin/bash
# curl and wget fail when the connection closes before the whole body arrived.

. ./helpers/httpd.sh
httpd_start
out=$(mktemp -d)

echo "=== curl complete download"
curl -s -o "$out/ok" "$BASE/ok"
echo "rc=$?"
cat "$out/ok"

echo "=== curl short Content-Length"
curl -s -o "$out/short" "$BASE/short"
echo "rc=$?"
wc -c < "$out/short"
curl -o "$out/short" "$BASE/short" 2>&1
echo "rc=$?"
curl -s "$BASE/short" > /dev/null && echo "chained command ran" || echo "chain stopped"

echo "=== curl truncated chunked body"
curl -s -o "$out/chunked" "$BASE/chunked-short"
echo "rc=$?"

echo "=== curl write error"
curl -s -o "$out/missing-dir/file" "$BASE/ok"
echo "rc=$?"

echo "=== wget complete download"
wget -q -O "$out/wok" "$BASE/ok"
echo "rc=$?"
cat "$out/wok"

echo "=== wget short Content-Length"
wget -q -O "$out/wshort" "$BASE/short" 2>&1 | sed "s|$out|OUT|"
wget -q -O "$out/wshort" "$BASE/short" 2> /dev/null
echo "rc=$?"

echo "=== wget truncated chunked body"
wget -q -O "$out/wchunked" "$BASE/chunked-short" 2> /dev/null
echo "rc=$?"

httpd_stop
rm -rf "$out"
