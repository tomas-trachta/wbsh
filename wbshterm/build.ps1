#Requires -Version 5.1
<#
.SYNOPSIS
    Builds wbshterm, Lua included, with the installed MSVC toolchain.
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

# Lua is C that MSVC's /W4 does not like; it gets its own pass without /WX.
function Invoke-LuaBuild {
    param([string]$LuaDir, [string]$OutputDir, [string[]]$Optimization)

    $luaOut = Join-Path $OutputDir 'lua'
    New-Item -ItemType Directory -Force -Path $luaOut | Out-Null
    $sources = Get-ChildItem -Path $LuaDir -Filter *.c | ForEach-Object { $_.FullName }

    $arguments = @('/nologo', '/c', '/W3', '/D_CRT_SECURE_NO_WARNINGS', "/Fo$luaOut\") + $Optimization + $sources
    & cl @arguments | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "cl.exe failed on Lua with exit code $LASTEXITCODE." }

    return Get-ChildItem -Path $luaOut -Filter *.obj | ForEach-Object { $_.FullName }
}

function Invoke-ClBuild {
    param([string]$SourceDir, [string]$OutputDir, [string]$Configuration)

    $optimization = if ($Configuration -eq 'Release') { '/O2', '/MD' } else { '/Od', '/Zi', '/MDd' }
    $sources = Get-ChildItem -Path $SourceDir -Filter *.cpp | ForEach-Object { $_.FullName }
    $luaDir = Join-Path (Split-Path -Parent $SourceDir) 'lua'
    $luaObjects = Invoke-LuaBuild -LuaDir $luaDir -OutputDir $OutputDir -Optimization $optimization

    $arguments = @(
        '/nologo', '/std:c++17', '/W4', '/WX', '/EHsc', '/permissive-', '/utf-8',
        '/D_CRT_SECURE_NO_WARNINGS', '/DWIN32_LEAN_AND_MEAN', '/DNOMINMAX', '/DUNICODE', '/D_UNICODE',
        "/I$luaDir", "/I$(Join-Path (Split-Path -Parent (Split-Path -Parent $SourceDir)) 'sdk\include')"
    ) + $optimization + $sources + $luaObjects + @(
        "/Fe:$(Join-Path $OutputDir 'wbshterm.exe')",
        "/Fo:$OutputDir\",
        '/link', '/SUBSYSTEM:WINDOWS', '/ENTRY:wWinMainCRTStartup',
        'kernel32.lib', 'user32.lib', 'gdi32.lib'
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
