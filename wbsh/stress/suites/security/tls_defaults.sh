#!/bin/bash
# curl and wget must refuse bad TLS certificates by default.
# Needs the internet: runs only when the driver gets -Net (STRESS_NET=1).
. "$STRESS_ROOT/lib/check.sh"

if [ "${STRESS_NET:-0}" != 1 ]; then
    skip "network checks need -Net"
    finish
fi
if ! curl -s --max-time 15 -o /dev/null https://badssl.com/; then
    skip "badssl.com is not reachable"
    finish
fi

severity critical
for host in expired self-signed wrong.host untrusted-root; do
    curl -s --max-time 15 -o /dev/null "https://$host.badssl.com/"
    check "curl refuses https://$host.badssl.com/" [ $? -ne 0 ]
    wget -q -T 15 -O /dev/null "https://$host.badssl.com/" 2>/dev/null
    check "wget refuses https://$host.badssl.com/" [ $? -ne 0 ]
done
severity low
curl -s -k --max-time 15 -o /dev/null https://self-signed.badssl.com/
check "control: curl -k accepts it" [ $? -eq 0 ]
finish
