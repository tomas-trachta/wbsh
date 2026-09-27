#Requires -Version 5.1
<#
.SYNOPSIS
    Downloads the pinned Microsoft.Windows.Console.ConPTY package and unpacks
    the x64 conpty.dll and OpenConsole.exe next to this script.

.DESCRIPTION
    wbshterm loads conpty.dll from its own directory when one is there and
    falls back to the pseudoconsole built into Windows otherwise. The
    package is Microsoft's current ConPTY, the same one Windows Terminal
    ships, and is far faster than the conhost frozen into Windows 10.

    The binaries are checked in so a clone builds without network access;
    run this script to refresh them after bumping $Version.
#>
[CmdletBinding()]
param(
    [string]$Version = '1.24.260710001'
)

$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$package = "microsoft.windows.console.conpty"
$url = "https://api.nuget.org/v3-flatcontainer/$package/$Version/$package.$Version.nupkg"
$archive = Join-Path $env:TEMP "$package.$Version.zip"

Write-Host "==> Downloading $url"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Invoke-WebRequest -Uri $url -OutFile $archive -UseBasicParsing

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($archive)
try {
    $wanted = @{
        'runtimes/win-x64/native/conpty.dll'       = 'conpty.dll'
        'build/native/runtimes/x64/OpenConsole.exe' = 'OpenConsole.exe'
    }

    foreach ($entry in $zip.Entries) {
        if (-not $wanted.ContainsKey($entry.FullName)) { continue }

        $target = Join-Path $here $wanted[$entry.FullName]
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
        Write-Host "    $($entry.FullName) -> $target ($($entry.Length) bytes)"
    }
} finally {
    $zip.Dispose()
    Remove-Item $archive -Force
}

Set-Content -Path (Join-Path $here 'VERSION.txt') -Value @(
    "Microsoft.Windows.Console.ConPTY $Version",
    "https://www.nuget.org/packages/Microsoft.Windows.Console.ConPTY/$Version",
    "https://github.com/microsoft/terminal",
    "License: MIT (LICENSE.txt)"
)
Write-Host "==> done"
