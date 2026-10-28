# Loopback HTTP server for the curl / wget tests. Listens on 127.0.0.1 on a
# free port, writes the port number to -PortFile, serves one request per
# connection and exits on GET /quit or after -TimeoutSeconds.
#
# Routes:
#   /ok              200 with a short text body
#   /short           Content-Length 100, sends 10 bytes, then closes
#   /chunked-short   chunked body that stops before the final chunk
#   /echo            200, body is the request head plus the request body
#   /status/<n>      status <n> with a short body
#   /cookie          sets the cookie session=abc123
#   /redirect        302 to /ok
#   /slow            answers after 3 seconds
#   /flaky           503 twice, then 200
#   /gzip            gzip-encoded body (Content-Encoding: gzip)
#   /range           20 bytes, honours "Range: bytes=N-"
#   /quit            200, then the server exits

param(
    [Parameter(Mandatory = $true)][string]$PortFile,
    [int]$TimeoutSeconds = 60
)

$ErrorActionPreference = 'Stop'
$latin1 = [System.Text.Encoding]::GetEncoding(28591)
$script:flakyCount = 0

function Read-Head($stream) {
    $head = New-Object System.Collections.Generic.List[byte]
    while ($true) {
        $b = $stream.ReadByte()
        if ($b -lt 0) { return $null }
        $head.Add([byte]$b)
        $n = $head.Count
        if ($n -ge 4 -and $head[$n - 4] -eq 13 -and $head[$n - 3] -eq 10 -and $head[$n - 2] -eq 13 -and $head[$n - 1] -eq 10) {
            return $latin1.GetString($head.ToArray())
        }
    }
}

function Read-Body($stream, [int]$length) {
    $body = New-Object byte[] $length
    $read = 0
    while ($read -lt $length) {
        $got = $stream.Read($body, $read, $length - $read)
        if ($got -le 0) { break }
        $read += $got
    }
    return ,$body
}

function Read-Request($stream) {
    $head = Read-Head $stream
    if ($null -eq $head) { return $null }

    $lines = $head -split "`r`n"
    $parts = $lines[0] -split ' '
    $headers = @{}
    foreach ($line in $lines | Select-Object -Skip 1) {
        if ($line -match '^([^:]+):\s*(.*)$') { $headers[$matches[1].ToLower()] = $matches[2] }
    }

    $body = New-Object byte[] 0
    if ($headers.ContainsKey('content-length')) { $body = Read-Body $stream ([int]$headers['content-length']) }
    return @{ Method = $parts[0]; Path = $parts[1]; Head = $head; Headers = $headers; Body = $body }
}

function Send($stream, [int]$status, [string]$reason, [string[]]$headers, [byte[]]$body, $declared = $null) {
    $length = if ($null -ne $declared) { $declared } else { $body.Length }
    $text = "HTTP/1.1 $status $reason`r`n"
    foreach ($h in $headers) { $text += "$h`r`n" }
    if ($length -ge 0) { $text += "Content-Length: $length`r`n" }
    $text += "Connection: close`r`n`r`n"

    $bytes = $latin1.GetBytes($text)
    $stream.Write($bytes, 0, $bytes.Length)
    if ($body.Length -gt 0) { $stream.Write($body, 0, $body.Length) }
    $stream.Flush()
}

function Send-Text($stream, [int]$status, [string]$reason, [string]$text, [string[]]$headers = @()) {
    Send $stream $status $reason (@('Content-Type: text/plain') + $headers) $latin1.GetBytes($text)
}

function Gzip([byte[]]$data) {
    $memory = New-Object System.IO.MemoryStream
    $gzip = New-Object System.IO.Compression.GZipStream($memory, [System.IO.Compression.CompressionMode]::Compress)
    $gzip.Write($data, 0, $data.Length)
    $gzip.Close()
    return ,$memory.ToArray()
}

function Send-Range($stream, $request) {
    $data = $latin1.GetBytes('0123456789abcdefghij')
    $range = $request.Headers['range']
    if ($range -match '^bytes=(\d+)-$') {
        $from = [int]$matches[1]
        if ($from -ge $data.Length) {
            Send-Text $stream 416 'Range Not Satisfiable' "out of range`n"
            return
        }
        $part = New-Object byte[] ($data.Length - $from)
        [Array]::Copy($data, $from, $part, 0, $part.Length)
        Send $stream 206 'Partial Content' @("Content-Range: bytes $from-$($data.Length - 1)/$($data.Length)") $part
        return
    }
    Send $stream 200 'OK' @() $data
}

function Handle($stream, $request) {
    $path = $request.Path -replace '^https?://[^/]+', ''
    switch -regex ($path) {
        '^/ok$' { Send-Text $stream 200 'OK' "hello from the test server`n"; break }
        '^/short$' { Send $stream 200 'OK' @() $latin1.GetBytes('0123456789') 100; break }
        '^/chunked-short$' {
            $bytes = $latin1.GetBytes("HTTP/1.1 200 OK`r`nTransfer-Encoding: chunked`r`nConnection: close`r`n`r`na`r`n0123456789`r`n")
            $stream.Write($bytes, 0, $bytes.Length)
            $stream.Flush()
            break
        }
        '^/echo' {
            $echo = $request.Head + $latin1.GetString($request.Body)
            Send-Text $stream 200 'OK' $echo
            break
        }
        '^/status/(\d+)$' { Send-Text $stream ([int]$matches[1]) 'Status' "status $($matches[1])`n"; break }
        '^/cookie$' { Send-Text $stream 200 'OK' "cookie set`n" @('Set-Cookie: session=abc123; Path=/'); break }
        '^/redirect$' { Send-Text $stream 302 'Found' "moved`n" @('Location: /ok'); break }
        '^/slow$' { Start-Sleep -Seconds 3; Send-Text $stream 200 'OK' "slow`n"; break }
        '^/flaky$' {
            $script:flakyCount++
            if ($script:flakyCount -le 2) { Send-Text $stream 503 'Unavailable' "try again`n" }
            else { Send-Text $stream 200 'OK' "recovered`n" }
            break
        }
        '^/gzip$' {
            $zipped = Gzip ($latin1.GetBytes("this body was gzip encoded`n"))
            Send $stream 200 'OK' @('Content-Type: text/plain', 'Content-Encoding: gzip') $zipped
            break
        }
        '^/range$' { Send-Range $stream $request; break }
        '^/quit$' { Send-Text $stream 200 'OK' "bye`n"; return $false }
        default { Send-Text $stream 404 'Not Found' "not found`n" }
    }
    return $true
}

$listener = New-Object System.Net.Sockets.TcpListener ([System.Net.IPAddress]::Loopback, 0)
$listener.Start()
[System.IO.File]::WriteAllText("$PortFile.tmp", [string]$listener.LocalEndpoint.Port)
Move-Item -Force "$PortFile.tmp" $PortFile

$deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
$running = $true
while ($running) {
    while (-not $listener.Pending()) {
        if ([DateTime]::UtcNow -gt $deadline) { $listener.Stop(); exit 0 }
        Start-Sleep -Milliseconds 10
    }

    $client = $listener.AcceptTcpClient()
    try {
        $stream = $client.GetStream()
        $stream.ReadTimeout = 5000
        $request = Read-Request $stream
        if ($null -ne $request) { $running = Handle $stream $request }
    } catch {
    } finally {
        $client.Close()
    }
}

$listener.Stop()
