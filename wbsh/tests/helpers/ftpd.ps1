# Minimal loopback FTP server for the net_tools test. Listens on 127.0.0.1
# on a free port (written to -PortFile), serves -Root, and handles one
# client at a time with passive data connections (EPSV and PASV). Supports
# USER/PASS, SYST, TYPE, PWD, CWD, LIST/NLST, RETR, STOR, DELE, MKD, RMD,
# RNFR/RNTO and QUIT. Exits after -TimeoutSeconds so a failed test leaves
# nothing spinning.

param(
    [Parameter(Mandatory = $true)][string]$PortFile,
    [Parameter(Mandatory = $true)][string]$Root,
    [int]$TimeoutSeconds = 60
)

$ErrorActionPreference = 'Stop'
$latin1 = [System.Text.Encoding]::GetEncoding(28591)
$loop = [System.Net.IPAddress]::Parse('127.0.0.1')

function Send-Line($stream, [string]$text) {
    $bytes = $latin1.GetBytes($text + "`r`n")
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush()
}

function Read-Line($stream) {
    $bytes = New-Object System.Collections.Generic.List[byte]
    while ($true) {
        $b = $stream.ReadByte()
        if ($b -lt 0) { return $null }
        if ($b -eq 10) { break }
        if ($b -ne 13) { $bytes.Add([byte]$b) }
    }
    return $latin1.GetString($bytes.ToArray())
}

function Open-Passive() {
    $listener = New-Object System.Net.Sockets.TcpListener($loop, 0)
    $listener.Start()
    return $listener
}

function Resolve-Path-Safe([string]$arg) {
    $name = $arg.Trim()
    if ($name.StartsWith('/')) { $name = $name.Substring(1) }
    return (Join-Path $Root $name)
}

function Accept-Data($listener) {
    $client = $listener.AcceptTcpClient()
    $listener.Stop()
    return $client
}

function Connect-Active($addr, $port) {
    $client = New-Object System.Net.Sockets.TcpClient
    $client.Connect($addr, [int]$port)
    return $client
}

$control = New-Object System.Net.Sockets.TcpListener($loop, 0)
$control.Start()
$port = $control.LocalEndpoint.Port
Set-Content -Path $PortFile -Value $port -NoNewline
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)

try {
    while ((Get-Date) -lt $deadline) {
        if (-not $control.Pending()) { Start-Sleep -Milliseconds 100; continue }

        $client = $control.AcceptTcpClient()
        $stream = $client.GetStream()
        $pending = $null
        $active = $null
        $renameFrom = $null
        try {
        Send-Line $stream '220 wbsh test ftpd'

        while ($true) {
            $line = Read-Line $stream
            if ($null -eq $line) { break }
            $parts = $line.Split(' ', 2)
            $cmd = $parts[0].ToUpper()
            $arg = if ($parts.Count -gt 1) { $parts[1] } else { '' }

            switch ($cmd) {
                'USER' { Send-Line $stream '331 need password' }
                'PASS' { Send-Line $stream '230 logged in' }
                'SYST' { Send-Line $stream '215 UNIX Type: L8' }
                'TYPE' { Send-Line $stream '200 type ok' }
                'PWD'  { Send-Line $stream '257 "/" is cwd' }
                'CWD'  { Send-Line $stream '250 cwd ok' }
                'EPSV' {
                    $active = $null
                    $pending = Open-Passive
                    $dp = $pending.LocalEndpoint.Port
                    Send-Line $stream "229 Entering Extended Passive Mode (|||$dp|)"
                }
                'PASV' {
                    $active = $null
                    $pending = Open-Passive
                    $dp = $pending.LocalEndpoint.Port
                    $hi = [math]::Floor($dp / 256)
                    $lo = $dp % 256
                    Send-Line $stream "227 Entering Passive Mode (127,0,0,1,$hi,$lo)"
                }
                'PORT' {
                    $n = $arg.Split(',')
                    $active = @{ Host = "$($n[0]).$($n[1]).$($n[2]).$($n[3])"; Port = ([int]$n[4] * 256 + [int]$n[5]) }
                    Send-Line $stream '200 PORT ok'
                }
                'EPRT' {
                    $f = $arg.Trim('|').Split('|')
                    $active = @{ Host = $f[1]; Port = [int]$f[2] }
                    Send-Line $stream '200 EPRT ok'
                }
                { $_ -eq 'LIST' -or $_ -eq 'NLST' } {
                    Send-Line $stream '150 here comes the listing'
                    $data = if ($active) { Connect-Active $active.Host $active.Port } else { Accept-Data $pending }
                    $ds = $data.GetStream()
                    foreach ($item in Get-ChildItem -LiteralPath $Root) {
                        $row = if ($cmd -eq 'NLST') { $item.Name } else { "-rw-r--r-- 1 u u $($item.Length) Jan 1 00:00 $($item.Name)" }
                        $bytes = $latin1.GetBytes($row + "`r`n")
                        $ds.Write($bytes, 0, $bytes.Length)
                    }
                    $ds.Flush(); $data.Close()
                    Send-Line $stream '226 listing done'
                }
                'RETR' {
                    $path = Resolve-Path-Safe $arg
                    if (-not (Test-Path -LiteralPath $path)) { Send-Line $stream '550 no such file'; break }
                    Send-Line $stream '150 sending'
                    $data = if ($active) { Connect-Active $active.Host $active.Port } else { Accept-Data $pending }
                    $ds = $data.GetStream()
                    $bytes = [System.IO.File]::ReadAllBytes($path)
                    $ds.Write($bytes, 0, $bytes.Length)
                    $ds.Flush(); $data.Close()
                    Send-Line $stream '226 transfer complete'
                }
                'STOR' {
                    $path = Resolve-Path-Safe $arg
                    Send-Line $stream '150 ready for data'
                    $data = if ($active) { Connect-Active $active.Host $active.Port } else { Accept-Data $pending }
                    $ds = $data.GetStream()
                    $out = [System.IO.File]::Create($path)
                    $buf = New-Object byte[] 65536
                    while ($true) {
                        $n = $ds.Read($buf, 0, $buf.Length)
                        if ($n -le 0) { break }
                        $out.Write($buf, 0, $n)
                    }
                    $out.Close(); $data.Close()
                    Send-Line $stream '226 stored'
                }
                'DELE' {
                    $path = Resolve-Path-Safe $arg
                    Remove-Item -LiteralPath $path -ErrorAction SilentlyContinue
                    Send-Line $stream '250 deleted'
                }
                'MKD' {
                    New-Item -ItemType Directory -Path (Resolve-Path-Safe $arg) -Force | Out-Null
                    Send-Line $stream '257 directory created'
                }
                'RMD' {
                    Remove-Item -LiteralPath (Resolve-Path-Safe $arg) -Recurse -ErrorAction SilentlyContinue
                    Send-Line $stream '250 removed'
                }
                'RNFR' { $renameFrom = Resolve-Path-Safe $arg; Send-Line $stream '350 ready for destination' }
                'RNTO' {
                    Rename-Item -LiteralPath $renameFrom -NewName (Split-Path -Leaf $arg) -ErrorAction SilentlyContinue
                    Send-Line $stream '250 renamed'
                }
                'QUIT' { Send-Line $stream '221 bye'; break }
                default { Send-Line $stream '502 not implemented' }
            }
        }
        } catch {
            # A client that resets the connection ends its own session only.
        } finally {
            $client.Close()
        }
    }
} finally {
    $control.Stop()
}
