#Requires -Version 5.1
<#
.SYNOPSIS
    Downloads the pinned Lua release and unpacks its interpreter sources
    next to this script.

.DESCRIPTION
    wbshterm embeds Lua for init.lua, the script that customises the
    terminal's chrome. The sources are checked in so a clone builds
    without network access and compile as part of wbshterm.vcxproj; the
    standalone interpreter (lua.c) and compiler (luac.c) are left out.

    Run this script to refresh them after bumping $Version, then update
    VERSION.txt and LICENSE.txt.
#>
[CmdletBinding()]
param(
    [string]$Version = '5.4.7'
)

$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$url = "https://www.lua.org/ftp/lua-$Version.tar.gz"
$archive = Join-Path $env:TEMP "lua-$Version.tar.gz"
$unpacked = Join-Path $env:TEMP "lua-$Version-src"

Write-Host "==> Downloading $url"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Invoke-WebRequest -Uri $url -OutFile $archive -UseBasicParsing

New-Item -ItemType Directory -Force -Path $unpacked | Out-Null
tar -xzf $archive -C $unpacked
if ($LASTEXITCODE -ne 0) { throw "tar failed with exit code $LASTEXITCODE." }

Get-ChildItem (Join-Path $here '*.c'), (Join-Path $here '*.h'), (Join-Path $here '*.hpp') | Remove-Item -Force
$source = Join-Path $unpacked "lua-$Version\src"
Get-ChildItem (Join-Path $source '*.c'), (Join-Path $source '*.h'), (Join-Path $source '*.hpp') |
    Where-Object { $_.Name -notin @('lua.c', 'luac.c') } |
    Copy-Item -Destination $here

Remove-Item -Recurse -Force $unpacked
Write-Host "==> Lua $Version sources are in $here"
