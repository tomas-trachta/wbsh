#Requires -Version 5.1
<#
.SYNOPSIS
    Smoke tests for the wbshterm M0 ConPTY spike.
.DESCRIPTION
    Each check drives a real wbsh session over a pseudoconsole and asserts
    on what came back through the pty, so a regression in handle setup or
    teardown fails here rather than in interactive use.
#>
[CmdletBinding()]
param(
    [string]$Spike
)

$ErrorActionPreference = 'Stop'
$script:Failures = 0

function Invoke-Spike {
    param([string[]]$SpikeArgs)

    $stdout = [System.IO.Path]::GetTempFileName()
    $stderr = [System.IO.Path]::GetTempFileName()
    $quoted = $SpikeArgs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }
    try {
        $process = Start-Process -FilePath $Spike -ArgumentList $quoted -NoNewWindow -Wait -PassThru `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            Stdout   = [string](Get-Content $stdout -Raw)
            Stderr   = [string](Get-Content $stderr -Raw)
        }
    }
    finally {
        Remove-Item $stdout, $stderr -ErrorAction SilentlyContinue
    }
}

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

function Test-CommandRoundTrip {
    $run = Invoke-Spike @('--feed', 'echo smoke-marker\rexit\r', '--delay', '400', '--timeout', '15000')
    Assert-That 'a fed command runs and its output comes back' ([bool]($run.Stdout -match 'smoke-marker'))
    Assert-That 'the shell exits cleanly' ($run.ExitCode -eq 0) "exit code $($run.ExitCode)"
}

function Test-NoOutputLeak {
    $run = Invoke-Spike @('--shell', "$env:SystemRoot\System32\cmd.exe", '--args', '/c echo leaked',
        '--feed', ' ', '--delay', '200', '--timeout', '10000', '--quiet')
    Assert-That 'child stdout goes to the pty, not to this process' (-not [bool]($run.Stdout -match 'leaked')) `
        "stdout was: $($run.Stdout)"
    Assert-That 'the pty carried the bytes' ([bool]($run.Stderr -match 'bytes in')) $run.Stderr
}

function Test-ExitStatusPropagates {
    $run = Invoke-Spike @('--feed', 'exit 7\r', '--delay', '400', '--timeout', '15000', '--quiet')
    Assert-That 'the shell exit code is reported' ([bool]($run.Stderr -match 'shell exit code 7')) $run.Stderr
}

if (-not $Spike) {
    $root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
    $Spike = Join-Path $root 'build\wbshterm-spike.exe'
}

if (-not (Test-Path $Spike)) {
    throw "spike not built: $Spike (run build.ps1 first)"
}

Test-CommandRoundTrip
Test-NoOutputLeak
Test-ExitStatusPropagates

if ($script:Failures -gt 0) {
    Write-Host "$script:Failures check(s) failed"
    exit 1
}

Write-Host 'all checks passed'
