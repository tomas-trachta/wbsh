#Requires -Version 5.1
<#
.SYNOPSIS
    Smoke tests for wbshterm.
.DESCRIPTION
    Drives the real binary: the built-in self-test covers the parser, the
    grid and one live pseudoconsole session; the checks here add the
    modes that only make sense from outside the process (snapshot,
    replay, and replay determinism).
#>
[CmdletBinding()]
param(
    [string]$Terminal
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

function Invoke-Terminal {
    param([string[]]$TerminalArgs)

    $quoted = $TerminalArgs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }
    $process = Start-Process -FilePath $Terminal -ArgumentList $quoted -NoNewWindow -Wait -PassThru
    return $process.ExitCode
}

function Test-SelfTest {
    param([string]$WorkDir)

    $report = Join-Path $WorkDir 'selftest.txt'
    $code = Invoke-Terminal @('--selftest', $report)

    $text = if (Test-Path $report) { Get-Content $report -Raw } else { '' }
    Assert-That 'the built-in self-test passes' ($code -eq 0) $text
    Assert-That 'the self-test report lists checks' ([bool]($text -match 'ok   ')) $text
}

function Test-Snapshot {
    param([string]$WorkDir)

    $image = Join-Path $WorkDir 'snapshot.png'
    $code = Invoke-Terminal @('--snapshot', $image, '--feed', 'echo snapshot-marker\r',
        '--size', '80x12', '--delay', '500', '--settle', '700')

    $bytes = if (Test-Path $image) { (Get-Item $image).Length } else { 0 }
    Assert-That 'a snapshot renders to a PNG' (($code -eq 0) -and ($bytes -gt 2000)) "$bytes bytes"
}

function Test-ReplayIsDeterministic {
    param([string]$WorkDir)

    $recording = Join-Path $WorkDir 'session.raw'
    $image = Join-Path $WorkDir 'recorded.png'
    Invoke-Terminal @('--snapshot', $image, '--record', $recording,
        '--feed', 'echo replay-marker\r', '--size', '80x12',
        '--delay', '500', '--settle', '700') | Out-Null

    if (-not (Test-Path $recording)) {
        Assert-That 'the session was recorded' $false 'no recording written'
        return
    }

    Assert-That 'the session was recorded' $true ''

    $first = Join-Path $WorkDir 'grid1.txt'
    $second = Join-Path $WorkDir 'grid2.txt'
    Invoke-Terminal @('--replay', $recording, '--dump', $first, '--size', '80x12') | Out-Null
    Invoke-Terminal @('--replay', $recording, '--dump', $second, '--size', '80x12') | Out-Null

    $gridOne = if (Test-Path $first) { Get-Content $first -Raw } else { 'one' }
    $gridTwo = if (Test-Path $second) { Get-Content $second -Raw } else { 'two' }

    Assert-That 'replaying a recording reproduces the grid' ($gridOne -eq $gridTwo) ''
    Assert-That 'the replayed grid holds the command output' `
        ([bool]($gridOne -match 'replay-marker')) $gridOne
}

if (-not $Terminal) {
    $root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
    $Terminal = Join-Path $root 'build\wbshterm.exe'
}

if (-not (Test-Path $Terminal)) {
    throw "wbshterm not built: $Terminal (run build.ps1 first)"
}

$workDir = Join-Path ([System.IO.Path]::GetTempPath()) ('wbshterm-smoke-' + [guid]::NewGuid())
New-Item -ItemType Directory -Force -Path $workDir | Out-Null

try {
    Test-SelfTest -WorkDir $workDir
    Test-Snapshot -WorkDir $workDir
    Test-ReplayIsDeterministic -WorkDir $workDir
}
finally {
    Remove-Item $workDir -Recurse -Force -ErrorAction SilentlyContinue
}

if ($script:Failures -gt 0) {
    Write-Host "$script:Failures check(s) failed"
    exit 1
}

Write-Host 'all checks passed'
