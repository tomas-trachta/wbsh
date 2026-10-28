# Tests for the networking builtins: nc, whois, httpd, ftp / ftpget / ftpput.
# Everything stays on 127.0.0.1 and reaches no network. Servers are bounded by
# their own timeouts and killed at the end, so a failing check never leaves a
# process spinning. Output is only PASS / FAIL lines so it is machine-stable.

fails=0

check() {
    local name=$1
    local got=$2
    local want=$3
    if [ "$got" = "$want" ]; then
        echo "PASS $name"
    else
        echo "FAIL $name (got [$got] want [$want])"
        fails=$((fails + 1))
    fi
}

lacks() {
    local name=$1
    local haystack=$2
    local needle=$3
    case "$haystack" in
        *"$needle"*) echo "FAIL $name (unexpected [$needle])"; fails=$((fails + 1)) ;;
        *) echo "PASS $name" ;;
    esac
}

contains() {
    local name=$1
    local haystack=$2
    local needle=$3
    case "$haystack" in
        *"$needle"*) echo "PASS $name" ;;
        *) echo "FAIL $name (missing [$needle] in [$haystack])"; fails=$((fails + 1)) ;;
    esac
}

base=$(( ($$ % 15000) + 41000 ))

wait_port() {
    local file=$1
    local tries=0
    while [ ! -s "$file" ] && [ "$tries" -lt 100 ]; do
        sleep 0.1
        tries=$((tries + 1))
    done
}

# -- nc ---------------------------------------------------------------------
nc_tests() {
    local p=$base
    printf 'NC-PAYLOAD\n' | nc -N -l 127.0.0.1 $p &
    local srv=$!
    sleep 0.6
    local got
    got=$(nc -w 3 127.0.0.1 $p)
    wait "$srv" 2>/dev/null
    check "nc roundtrip" "$got" "NC-PAYLOAD"

    local q=$((base + 1))
    printf 'x\n' | nc -N -l 127.0.0.1 $q &
    srv=$!
    sleep 0.6
    nc -z -w 2 127.0.0.1 $q >/dev/null 2>&1
    check "nc -z open port" "$?" "0"
    wait "$srv" 2>/dev/null

    nc -z -w 2 127.0.0.1 $((base + 2)) >/dev/null 2>&1
    check "nc -z closed port" "$?" "1"

    local u=$((base + 9))
    printf 'UDP-REPLY\n' | nc -u -q 0 -w 5 -l 127.0.0.1 $u >/dev/null 2>&1 &
    srv=$!
    sleep 0.6
    got=$(printf 'ping\n' | nc -u -w 2 127.0.0.1 $u)
    wait "$srv" 2>/dev/null
    check "nc udp roundtrip" "$got" "UDP-REPLY"
}

# -- whois ------------------------------------------------------------------
whois_tests() {
    local p=$((base + 3))
    printf 'Domain: EXAMPLE.TEST\r\nRegistrar: ACME Inc\r\n' | nc -q 0 -l 127.0.0.1 $p >/dev/null 2>&1 &
    local srv=$!
    sleep 0.6
    local out
    out=$(whois -h 127.0.0.1 -p $p example.test)
    wait "$srv" 2>/dev/null
    contains "whois -h reply" "$out" "Registrar: ACME Inc"
}

# -- httpd ------------------------------------------------------------------
httpd_tests() {
    local root
    root=$(mktemp -d)
    printf '<h1>home</h1>\n' > "$root/index.html"
    printf 'abcdefghijklmnopqrstuvwxyz' > "$root/data.txt"
    mkdir "$root/sub"
    printf 'leaf\n' > "$root/sub/leaf.txt"
    printf 'amp\n' > "$root/sub/a&b'c.txt"
    printf 'accent\n' > "$root/sub/é.txt"

    local p=$((base + 4))
    httpd -f -p 127.0.0.1:$p -h "$root" &
    local srv=$!
    sleep 0.8

    contains "httpd index" "$(curl -s http://127.0.0.1:$p/)" "home"
    check "httpd file" "$(curl -s http://127.0.0.1:$p/data.txt)" "abcdefghijklmnopqrstuvwxyz"
    check "httpd range" "$(curl -s -r 5-9 http://127.0.0.1:$p/data.txt)" "fghij"
    check "httpd 404" "$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:$p/nope)" "404"
    check "httpd traversal" "$(curl -s -o /dev/null -w '%{http_code}' --path-as-is http://127.0.0.1:$p/../data.txt)" "403"
    check "httpd method" "$(curl -s -o /dev/null -w '%{http_code}' -X PUT http://127.0.0.1:$p/data.txt)" "501"
    local listing
    listing=$(curl -s http://127.0.0.1:$p/sub/)
    contains "httpd listing" "$listing" "leaf.txt"
    contains "httpd listing escaped text" "$listing" ">a&amp;b&#39;c.txt<"
    contains "httpd listing encoded href" "$listing" 'href="a%26b%27c.txt"'
    lacks "httpd listing no raw name" "$listing" "a&b'c.txt"
    check "httpd utf8 file" "$(curl -s http://127.0.0.1:$p/sub/%C3%A9.txt)" "accent"
    contains "httpd utf8 listing href" "$listing" 'href="%C3%A9.txt"'
    contains "httpd utf8 listing text" "$listing" ">é.txt<"
    local clen
    clen=$(curl -sI http://127.0.0.1:$p/data.txt | tr -d '\r' | awk 'tolower($1)=="content-length:"{print $2}')
    check "httpd HEAD length" "$clen" "26"

    kill "$srv" 2>/dev/null
    rm -rf "$root"
}

# -- httpd CGI --------------------------------------------------------------
httpd_cgi_tests() {
    local root
    root=$(mktemp -d)
    mkdir "$root/cgi-bin"
    printf '#!/bin/sh\n' > "$root/cgi-bin/echo"
    printf 'printf "Content-Type: text/plain\\r\\n\\r\\n"\n' >> "$root/cgi-bin/echo"
    printf 'printf "method=%%s query=%%s\\n" "$REQUEST_METHOD" "$QUERY_STRING"\n' >> "$root/cgi-bin/echo"
    printf 'cat\n' >> "$root/cgi-bin/echo"
    printf '#!/bin/sh\nprintf "Status: 302 Found\\r\\nLocation: /data.txt\\r\\n\\r\\n"\n' > "$root/cgi-bin/go"
    printf 'PLAIN-SOURCE\n' > "$root/cgi-bin/plain.txt"
    printf 'redirect-body\n' > "$root/data.txt"

    local p=$((base + 5))
    httpd -f -p 127.0.0.1:$p -h "$root" &
    local srv=$!
    sleep 0.8

    contains "cgi GET query" "$(curl -s 'http://127.0.0.1:'$p'/cgi-bin/echo?foo=bar')" "method=GET query=foo=bar"
    contains "cgi POST body" "$(curl -s --data 'hello=world' http://127.0.0.1:$p/cgi-bin/echo)" "hello=world"
    check "cgi redirect status" "$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:$p/cgi-bin/go)" "302"

    local out
    out=$(curl -s --path-as-is --data 'echo MARKER' -w ' %{http_code}' \
        "http://127.0.0.1:$p/cgi-bin/C:%5CWindows%5CSystem32%5Ccmd.exe")
    lacks "cgi absolute encoded no exec" "$out" "MARKER"
    contains "cgi absolute encoded refused" "$out" " 403"
    out=$(curl -s --path-as-is --data 'echo MARKER' -w ' %{http_code}' \
        "http://127.0.0.1:$p/cgi-bin/C:\\Windows\\System32\\cmd.exe")
    lacks "cgi absolute raw no exec" "$out" "MARKER"
    out=$(curl -s --path-as-is -w ' %{http_code}' "http://127.0.0.1:$p/cgi-bin/plain.txt")
    lacks "cgi plain file not served" "$out" "PLAIN-SOURCE"
    contains "cgi plain file refused" "$out" " 403"
    out=$(curl -s --path-as-is -w ' %{http_code}' "http://127.0.0.1:$p/CGI-BIN/plain.txt")
    lacks "cgi case variant not served" "$out" "PLAIN-SOURCE"

    kill "$srv" 2>/dev/null
    rm -rf "$root"
}

# -- httpd -c config (allow/deny, basic auth, mime, index, error page) ------
httpd_conf_tests() {
    local root
    root=$(mktemp -d)
    printf 'hidden\n' > "$root/secret.txt"
    mkdir "$root/vault"
    printf 'vaulted\n' > "$root/vault/s.txt"
    printf 'foo-body\n' > "$root/thing.foo"
    printf '<h1>welcome</h1>\n' > "$root/home.htm"
    printf 'CUSTOM-404\n' > "$root/e404.html"
    printf 'A:127.0.0.1\nD:*\n/secret.txt:bob:pw\n/vault:bob:pw\n.foo:application/x-foo\nI:home.htm\nE404:/e404.html\n' > "$root/httpd.conf"

    local p=$((base + 6))
    httpd -f -c "$root/httpd.conf" -p 127.0.0.1:$p -h "$root" &
    local srv=$!
    sleep 0.8

    check "conf auth 401" "$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:$p/secret.txt)" "401"
    check "conf auth ok" "$(curl -s -u bob:pw http://127.0.0.1:$p/secret.txt)" "hidden"
    local variant
    for variant in /VAULT/s.txt /%76ault/s.txt /./vault/s.txt '/vault\s.txt'; do
        check "conf auth bypass $variant" \
            "$(curl -s --path-as-is -o /dev/null -w '%{http_code}' "http://127.0.0.1:$p$variant")" "401"
    done
    check "conf auth trailing dot refused" \
        "$(curl -s --path-as-is -o /dev/null -w '%{http_code}' "http://127.0.0.1:$p/vault./s.txt")" "403"
    check "conf auth dir ok" "$(curl -s -u bob:pw http://127.0.0.1:$p/vault/s.txt)" "vaulted"
    local ct
    ct=$(curl -sI http://127.0.0.1:$p/thing.foo | tr -d '\r' | awk 'tolower($1)=="content-type:"{print $2}')
    check "conf mime override" "$ct" "application/x-foo"
    contains "conf index name" "$(curl -s http://127.0.0.1:$p/)" "welcome"
    contains "conf error page" "$(curl -s http://127.0.0.1:$p/missing)" "CUSTOM-404"

    kill "$srv" 2>/dev/null
    rm -rf "$root"

    # A separate server that denies the loopback client outright.
    local droot
    droot=$(mktemp -d)
    printf 'x\n' > "$droot/a.txt"
    printf 'D:127.0.0.1\n' > "$droot/httpd.conf"
    local dp=$((base + 7))
    httpd -f -c "$droot/httpd.conf" -p 127.0.0.1:$dp -h "$droot" &
    srv=$!
    sleep 0.8
    check "conf deny ip" "$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:$dp/a.txt)" "403"
    kill "$srv" 2>/dev/null
    rm -rf "$droot"
}

# -- ftp / ftpget / ftpput --------------------------------------------------
start_ftpd() {
    local root=$1
    local pf=$2
    powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden \
        -File helpers/ftpd.ps1 -PortFile "$pf" -Root "$root" -TimeoutSeconds 60 &
    FTPD_PID=$!
    wait_port "$pf"
}

ftp_tests() {
    local root work pf port
    root=$(mktemp -d)
    work=$(mktemp -d)
    pf="$work/port"
    printf 'FTP-PAYLOAD\n' > "$root/remote.txt"
    start_ftpd "$root" "$pf"
    port=$(cat "$pf")

    ftpget -P "$port" 127.0.0.1 "$work/got.txt" remote.txt >/dev/null 2>&1
    check "ftpget" "$(cat "$work/got.txt" 2>/dev/null)" "FTP-PAYLOAD"

    printf 'UP-PAYLOAD\n' > "$work/up.txt"
    ftpput -P "$port" 127.0.0.1 stored.txt "$work/up.txt" >/dev/null 2>&1
    check "ftpput" "$(cat "$root/stored.txt" 2>/dev/null)" "UP-PAYLOAD"

    local listing
    listing=$(ftp -n 127.0.0.1 "$port" <<FTP 2>/dev/null
user anonymous x@x
binary
dir
quit
FTP
)
    contains "ftp dir" "$listing" "remote.txt"

    # Active mode (PORT): the data listener binds the control connection's
    # own local address, so loopback never touches 0.0.0.0.
    local active_listing
    active_listing=$(ftp -n 127.0.0.1 "$port" <<FTP 2>/dev/null
user anonymous x@x
passive
dir
quit
FTP
)
    contains "ftp active dir" "$active_listing" "remote.txt"

    # ftp:// URL autofetch and ftpget URL operand.
    cd "$work"
    ftp -n "ftp://127.0.0.1:$port/remote.txt" >/dev/null 2>&1
    check "ftp url autofetch" "$(cat "$work/remote.txt" 2>/dev/null)" "FTP-PAYLOAD"
    cd - >/dev/null
    ftpget "ftp://127.0.0.1:$port/remote.txt" "$work/url.txt" >/dev/null 2>&1
    check "ftpget url" "$(cat "$work/url.txt" 2>/dev/null)" "FTP-PAYLOAD"

    kill "$FTPD_PID" 2>/dev/null
    rm -rf "$root" "$work"
}

nc_tests
whois_tests
httpd_tests
httpd_cgi_tests
httpd_conf_tests
ftp_tests

echo "net_tools: $fails failures"
[ "$fails" -eq 0 ]
