# Sourced by the curl / wget tests: starts helpers/httpd.ps1 on a loopback
# port and sets BASE to its URL. httpd_stop asks it to exit and waits.

httpd_start() {
    HTTPD_DIR=$(mktemp -d)
    powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass \
        -File helpers/httpd.ps1 -PortFile "$HTTPD_DIR/port" -TimeoutSeconds 90 &
    HTTPD_PID=$!

    local tries
    tries=0
    while [ ! -s "$HTTPD_DIR/port" ] && [ "$tries" -lt 300 ]; do
        sleep 0.1
        tries=$((tries + 1))
    done

    BASE="http://127.0.0.1:$(cat "$HTTPD_DIR/port")"
}

httpd_stop() {
    curl -s -m 10 "$BASE/quit" > /dev/null
    wait "$HTTPD_PID"
    rm -rf "$HTTPD_DIR"
}
