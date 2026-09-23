#Requires -Version 5.1
<#
.SYNOPSIS
    Builds the wbshterm M0 ConPTY spike with the installed MSVC toolchain.
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

function Find-VcVarsScript {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw 'vswhere.exe not found; install Visual Studio 2022 with the C++ workload.'
    }

    $root = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if (-not $root) { throw 'No Visual Studio installation with the C++ toolset was found.' }

    $vcvars = Join-Path $root 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found under $root." }

    return $vcvars
}

function Import-VcEnvironment {
    param([string]$VcVarsScript)

    cmd /c "`"$VcVarsScript`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') {
            Set-Item -Path "env:$($matches[1])" -Value $matches[2]
        }
    }
}

function Invoke-ClBuild {
    param([string]$SourceDir, [string]$OutputDir, [string]$Configuration)

    $optimization = if ($Configuration -eq 'Release') { '/O2', '/MD' } else { '/Od', '/Zi', '/MDd' }
    $sources = Get-ChildItem -Path $SourceDir -Filter *.cpp | ForEach-Object { $_.FullName }

    $arguments = @(
        '/nologo', '/std:c++17', '/W4', '/WX', '/EHsc', '/permissive-', '/utf-8',
        '/D_CRT_SECURE_NO_WARNINGS', '/DWIN32_LEAN_AND_MEAN', '/DNOMINMAX', '/DUNICODE', '/D_UNICODE'
    ) + $optimization + $sources + @(
        "/Fe:$(Join-Path $OutputDir 'wbshterm.exe')",
        "/Fo:$OutputDir\",
        '/link', '/SUBSYSTEM:WINDOWS', '/ENTRY:wWinMainCRTStartup',
        'kernel32.lib', 'user32.lib'
    )

    & cl @arguments
    if ($LASTEXITCODE -ne 0) { throw "cl.exe failed with exit code $LASTEXITCODE." }
}

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$outputDir = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

Import-VcEnvironment (Find-VcVarsScript)
Invoke-ClBuild -SourceDir (Join-Path $root 'src') -OutputDir $outputDir -Configuration $Configuration

Write-Host "built $(Join-Path $outputDir 'wbshterm.exe')"
