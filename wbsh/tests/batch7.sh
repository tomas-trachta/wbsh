#!/bin/bash
echo "=== diff -u"
printf 'one\ntwo\nthree\nfour\nfive\n' > /tmp/wbsh-d-a
printf 'one\nTWO\nthree\nfour\nFIVE\nsix\n' > /tmp/wbsh-d-b
diff -u /tmp/wbsh-d-a /tmp/wbsh-d-b
rm -f /tmp/wbsh-d-a /tmp/wbsh-d-b

echo "=== curl"
# Use a stable static endpoint that returns predictable text.
curl -s https://example.com | head -2
echo --
curl -sI https://example.com | head -1
echo --
curl -s -o /tmp/wbsh-curl-out.html https://example.com
wc -c /tmp/wbsh-curl-out.html
rm -f /tmp/wbsh-curl-out.html

echo "=== background & + jobs + wait"
sleep 0.3 &
sleep 0.5 &
echo "spawned two background sleeps; \$!=$!"
jobs
wait
jobs
echo "all reaped"

echo "=== done"
