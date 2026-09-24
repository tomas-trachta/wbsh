#Requires -Version 5.1
<#
.SYNOPSIS
    Integration checks for the wbsh SDK.
.DESCRIPTION
    Drives the real binaries with the sample util installed: it must be
    listed, its command must behave like any other command, and a DLL that
    is not a util must be skipped without taking the good one with it.
#>
[CmdletBinding()]
param(
    [string]$Shell,
    [string]$PluginDir
)

$ErrorActionPreference = 'Stop'
$script:Failures = 0

function Assert-That {
    param([string]$Name, [bool]$Condition, [string]$Detail = '')

    if ($Condition) {
        Write-Host "ok   $Name"
        return
    }

    Write-Host "FAIL $Name"
    if ($Detail) { Write-Host "     $Detail" }
    $script:Failures++
}

# Stderr from a native program arrives as an error record, and a util that
# is skipped is expected to say so there, so this one call stops treating
# that as fatal.
function Invoke-Shell {
    param([string]$Script)

    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        return (& $Shell -r -c $Script 2>&1 | Out-String)
    }
    finally {
        $ErrorActionPreference = $previous
    }
}

if (-not $Shell) {
    $root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
    $Shell = Join-Path $root 'x64\Release\wbsh.exe'
}
if (-not $PluginDir) {
    $PluginDir = Join-Path (Split-Path -Parent $Shell) 'plugins'
}

if (-not (Test-Path $Shell)) { throw "wbsh not built: $Shell" }

Write-Host "shell:   $Shell"
Write-Host "plugins: $PluginDir"

$listed = Invoke-Shell 'utils'
Assert-That 'the sample util is listed by `utils`' ($listed -match 'hello') $listed
Assert-That 'the listing carries its version and summary' `
    (($listed -match '1\.0\.0') -and ($listed -match 'worked example')) $listed

$greeting = Invoke-Shell 'hello Tomas'
Assert-That 'a util command runs' ($greeting -match 'Hello, Tomas!') $greeting

$piped = Invoke-Shell 'hello Tomas | tr a-z A-Z'
Assert-That 'a util command works in a pipeline' ($piped -match 'HELLO, TOMAS!') $piped

$status = Invoke-Shell 'hello >nul; echo status=$?'
Assert-That 'a util command sets the exit status' ($status -match 'status=0') $status

$where = Invoke-Shell 'hello --where'
Assert-That 'a util can ask the host where it is' ($where -match 'You are in') $where

$shadow = Invoke-Shell 'echo marker | wc -l'
Assert-That 'the bundled commands are untouched by any of this' ($shadow -match '1') $shadow

# A DLL that exports nothing must be skipped, and the good util must still
# load: one bad file in the folder cannot cost the others.
$decoy = Join-Path $PluginDir 'not-a-util.dll'
Copy-Item (Join-Path (Split-Path -Parent $Shell) 'wbshsdk.dll') $decoy -Force
try {
    $withDecoy = Invoke-Shell 'utils'
    Assert-That 'a DLL that is not a util does not stop the others' `
        ($withDecoy -match 'hello') $withDecoy
}
finally {
    Remove-Item $decoy -Force -ErrorAction SilentlyContinue
}

if ($script:Failures -gt 0) {
    Write-Host "$script:Failures check(s) failed"
    exit 1
}

Write-Host 'all checks passed'
