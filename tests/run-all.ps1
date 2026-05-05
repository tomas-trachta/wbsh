# Wrapper around tests/run-all.sh for PowerShell users.
#
# Usage:
#     .\tests\run-all.ps1
#     .\tests\run-all.ps1 -Golden        # also diff vs expected/
#     .\tests\run-all.ps1 -Record        # capture expected/
[CmdletBinding()]
param(
    [switch]$Golden,
    [switch]$Record
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'x64\Release\wbsh.exe'
if (-not (Test-Path $exe)) {
    $exe = Join-Path $root 'x64\Debug\wbsh.exe'
}
if (-not (Test-Path $exe)) {
    Write-Error "wbsh.exe not found under $root\x64\{Release,Debug}; build the project first."
}

if ($Record) { $env:WBSH_RECORD = '1' }
if ($Golden) { $env:WBSH_GOLDEN = '1' }

Push-Location $PSScriptRoot
try {
    & $exe -r run-all.sh
    $rc = $LASTEXITCODE
} finally {
    Pop-Location
    Remove-Item Env:WBSH_RECORD -ErrorAction SilentlyContinue
    Remove-Item Env:WBSH_GOLDEN -ErrorAction SilentlyContinue
}
exit $rc
