#!/bin/bash
# curl options against a loopback server: option parsing, -f, -d, -L, headers,
# -w, timeouts, retries, forms, resume, outputs, cookies, verbose output.

. ./helpers/httpd.sh
httpd_start
tmp=$(mktemp -d)

show() {
    sed -e 's/127\.0\.0\.1:[0-9]*/HOST/g' -e 's/-\{20,\}[0-9a-f]*/BOUNDARY/g' | tr -d '\r'
}

echo "=== unknown options are rejected"
curl --bogus "$BASE/ok"
echo "rc=$?"
curl -z "$BASE/ok"
echo "rc=$?"
curl -o
echo "rc=$?"
curl -s
echo "rc=$?"

echo "=== option values are not taken as the URL"
curl -sS "$BASE/ok" --retry 3
echo "rc=$?"

echo "=== HTTP errors fail only with -f"
curl -s "$BASE/status/404"
echo "rc=$?"
curl -sS -f "$BASE/status/404"
echo "rc=$?"
curl -s -f "$BASE/status/404"
echo "rc=$?"
curl -s --fail-with-body "$BASE/status/500"
echo "rc=$?"
curl -sf -o "$tmp/none" "$BASE/status/404"
echo "rc=$?"
[ -e "$tmp/none" ] && echo "file created" || echo "no file"

echo "=== request bodies"
curl -s -d 'a=1' -d 'b=2' "$BASE/echo" | show
echo
printf 'line1\nline2\n' > "$tmp/data"
curl -s -d "@$tmp/data" "$BASE/echo" | tail -1
echo
curl -s --data-binary "@$tmp/data" "$BASE/echo" | tail -2
curl -s --data-urlencode 'q=a b&c' "$BASE/echo" | tail -1
echo
curl -s -H 'Content-Type: text/plain' -d 'x' "$BASE/echo" | grep Content-Type | show
curl -s --json '{"a":1}' "$BASE/echo" | grep -E 'Content-Type|Accept' | show
curl -s -G -d 'q=1' -d 'r=2' "$BASE/echo" | head -1 | show
curl -s -X PUT -d 'x' "$BASE/echo" | head -1 | show
curl -s -T "$tmp/data" "$BASE/echo" | show
echo "=== redirects are followed only with -L"
curl -s "$BASE/redirect"
echo "rc=$?"
curl -sL "$BASE/redirect"
curl -siL "$BASE/redirect" | show
curl -sS -L --max-redirs 0 "$BASE/redirect"
echo "rc=$?"
curl -s -L -d x=1 "$BASE/redirect"

echo "=== request headers"
curl -s -u user:pass -A agent/1 -e http://ref/ -H 'X-Test: yes' "$BASE/echo" | show
curl -s -H 'User-Agent:' -H 'Accept:' "$BASE/echo" | show

echo "=== write-out"
curl -s -o /dev/null -w '%{http_code} %{num_redirects} %{content_type} %{size_download}\n' -L "$BASE/redirect"
curl -s -o /dev/null -w '%{response_code}|%header{content-type}|%{exitcode}|%%\n' "$BASE/ok"
curl -sf -w '%{onerror}failed with %{exitcode}\n' "$BASE/status/404"
curl -s -o /dev/null -w '%{nosuchvar}\n' "$BASE/ok"

echo "=== timeouts and retries"
curl -s -m 1 "$BASE/slow"
echo "rc=$?"
curl -sS --retry 3 "$BASE/flaky"
echo "rc=$?"

echo "=== proxy"
curl -s -x "$BASE" http://example.invalid/echo | head -1

echo "=== multipart forms"
printf 'file content\n' > "$tmp/up.txt"
curl -s -F 'name=value' -F "up=@$tmp/up.txt" -F "note=<$tmp/up.txt" "$BASE/echo" | show

echo "=== compressed, resume, range"
curl -s --compressed "$BASE/gzip"
printf '01234' > "$tmp/part"
curl -s -C - -o "$tmp/part" "$BASE/range"
echo "rc=$?"
cat "$tmp/part"
echo
curl -s -r 15- "$BASE/range"
echo

echo "=== outputs"
curl -s --create-dirs -o "$tmp/a/b/c.txt" "$BASE/ok"
cat "$tmp/a/b/c.txt"
(cd "$tmp" && curl -s -O "$BASE/ok" && cat ok)
curl -s "$BASE/ok" "$BASE/status/201"
curl -s -o "$tmp/one" -o "$tmp/two" "$BASE/ok" "$BASE/status/202"
cat "$tmp/one" "$tmp/two"

echo "=== cookies"
curl -s -b 'k=v' "$BASE/echo" | grep Cookie | show
curl -s -c "$tmp/jar" "$BASE/cookie" > /dev/null
grep session "$tmp/jar" | cut -f1,6,7
curl -s -b "$tmp/jar" "$BASE/echo" | grep Cookie | show

echo "=== verbose, dump-header, head"
curl -sv "$BASE/ok" 2>&1 >/dev/null | grep -E '^[<>]' | show
curl -s -D - -o /dev/null "$BASE/ok" | show
curl -sI "$BASE/ok" | show

echo "=== errors"
curl -sS http://127.0.0.1:1/ 2>&1 | sed 's/after [0-9]* ms/after N ms/'
curl -s http://127.0.0.1:1/
echo "rc=$?"
curl -sS ftp://example.invalid/
echo "rc=$?"
curl -s file:///nonexistent/file
echo "rc=$?"
curl -s "file://$tmp/up.txt"

echo "=== config file"
printf 'silent\nheader = "X-Config: 1"\nurl = %s/echo\n' "$BASE" > "$tmp/cfg"
curl -K "$tmp/cfg" | grep X-Config | show

httpd_stop
rm -rf "$tmp"
