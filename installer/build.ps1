# installer/build.ps1 -- produce wbsh installer .exe + portable .zip.
#
# Steps:
#   1. Locate MSBuild via vswhere.
#   2. Build wbsh.vcxproj in Release|x64.
#   3. Stage wbsh.exe, wbsh-here.cmd, and the VC++ runtime DLLs into stage\.
#   4. Compile installer\wbsh.iss with ISCC, emitting output\wbsh-setup-x64.exe.
#   5. Zip the staged tree into output\wbsh-<ver>-portable-x64.zip.
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$Platform      = 'x64',
    # Defaults to the WbshVersion property in wbsh.vcxproj (the single
    # source of truth). Pass -Version X.Y.Z to override for a one-off
    # build without editing the project file.
    [string]$Version       = ''
)

$ErrorActionPreference = 'Stop'
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = Resolve-Path (Join-Path $ScriptDir '..')

# Read the version from wbsh.vcxproj if the caller didn't override.
# We read the three leaf properties rather than the composite
# WbshVersion: the composite is stored in XML as the literal string
# "$(WbshVersionMajor).$(WbshVersionMinor).$(WbshVersionPatch)" — those
# macro references are evaluated by MSBuild at build time, not by
# PowerShell when it parses the XML.
if (-not $Version) {
    $projPath = Join-Path $RepoRoot 'wbsh.vcxproj'
    $projXml  = [xml](Get-Content $projPath)
    $vMajor = $null; $vMinor = $null; $vPatch = $null
    foreach ($pg in $projXml.Project.PropertyGroup) {
        if ($pg.WbshVersionMajor) { $vMajor = $pg.WbshVersionMajor.Trim() }
        if ($pg.WbshVersionMinor) { $vMinor = $pg.WbshVersionMinor.Trim() }
        if ($pg.WbshVersionPatch) { $vPatch = $pg.WbshVersionPatch.Trim() }
    }
    if ($null -eq $vMajor -or $null -eq $vMinor -or $null -eq $vPatch) {
        throw "Could not locate WbshVersionMajor/Minor/Patch in $projPath. Either set them there or pass -Version."
    }
    $Version = "$vMajor.$vMinor.$vPatch"
}

# --- Locate MSBuild via vswhere -------------------------------------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found at $vswhere -- install Visual Studio Build Tools."
}
$vsRoot = & $vswhere -latest -products * `
    -requires Microsoft.Component.MSBuild `
    -property installationPath
if (-not $vsRoot) { throw "No Visual Studio instance with MSBuild found." }
$msbuild = Join-Path $vsRoot 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path $msbuild)) { throw "MSBuild not found at $msbuild." }

# --- Locate ISCC (Inno Setup compiler) ------------------------------------
$iscc = $null
$candidates = @(
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "${env:ProgramFiles}\Inno Setup 6\ISCC.exe",
    "${env:LOCALAPPDATA}\Programs\Inno Setup 6\ISCC.exe"
)
foreach ($p in $candidates) {
    if (Test-Path $p) { $iscc = $p; break }
}
if (-not $iscc) {
    $cmd = Get-Command iscc -ErrorAction SilentlyContinue
    if ($cmd) { $iscc = $cmd.Source }
}
if (-not $iscc) {
    Write-Warning "ISCC.exe not found -- skipping installer build. Portable ZIP will still be produced."
    Write-Warning "Install Inno Setup 6 from https://jrsoftware.org/isdl.php (or: winget install JRSoftware.InnoSetup)."
}

# --- Build ----------------------------------------------------------------
Write-Host "==> Building wbsh $Version ($Configuration|$Platform)" -ForegroundColor Cyan

# Split version into major.minor.patch so the VERSIONINFO numeric tuple
# (FILEVERSION / PRODUCTVERSION in wbsh.rc) matches the string form.
# Passing all four properties as /p: overrides keeps them in lockstep
# regardless of what's in the vcxproj when -Version was supplied.
$parts = $Version.Split('.')
if ($parts.Count -lt 3) { throw "Version must be major.minor.patch, got: $Version" }
$VerMajor = $parts[0]; $VerMinor = $parts[1]; $VerPatch = $parts[2]

& $msbuild (Join-Path $RepoRoot 'wbsh.vcxproj') `
    -nologo "-p:Configuration=$Configuration" "-p:Platform=$Platform" `
    "-p:WbshVersion=$Version" `
    "-p:WbshVersionMajor=$VerMajor" `
    "-p:WbshVersionMinor=$VerMinor" `
    "-p:WbshVersionPatch=$VerPatch" `
    -v:minimal
if ($LASTEXITCODE -ne 0) { throw "MSBuild failed (exit $LASTEXITCODE)." }

$exePath = Join-Path $RepoRoot "$Platform\$Configuration\wbsh.exe"
if (-not (Test-Path $exePath)) { throw "Build did not produce $exePath." }

# --- Stage payload --------------------------------------------------------
Write-Host "==> Staging payload" -ForegroundColor Cyan
$stage = Join-Path $ScriptDir 'stage'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage | Out-Null

Copy-Item $exePath                                     $stage
Copy-Item (Join-Path $ScriptDir 'wbsh-here.cmd')       $stage

# Copy VC++ runtime DLLs app-local. Required because the project links the
# dynamic CRT (/MD); without these, wbsh.exe will fail to start on machines
# that don't already have the matching VC redistributable.
$redistRoot = Join-Path $vsRoot 'VC\Redist\MSVC'
if (Test-Path $redistRoot) {
    $latestVer = Get-ChildItem $redistRoot -Directory |
        Where-Object { $_.Name -match '^\d' } |
        Sort-Object Name -Descending | Select-Object -First 1
    if ($latestVer) {
        $crtDir = Join-Path $latestVer.FullName "$Platform\Microsoft.VC143.CRT"
        if (Test-Path $crtDir) {
            foreach ($dll in @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
                $src = Join-Path $crtDir $dll
                if (Test-Path $src) { Copy-Item $src $stage }
                else { Write-Warning "Missing runtime DLL: $src" }
            }
        } else {
            Write-Warning "VC redist CRT directory not found: $crtDir"
        }
    }
} else {
    Write-Warning "VC redist root not found: $redistRoot"
}

# --- Portable ZIP ---------------------------------------------------------
Write-Host "==> Producing portable ZIP" -ForegroundColor Cyan
$outDir = Join-Path $ScriptDir 'output'
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$zipPath = Join-Path $outDir "wbsh-$Version-portable-$Platform.zip"
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zipPath
Write-Host "    portable -> $zipPath" -ForegroundColor Green

# --- Installer ------------------------------------------------------------
if ($iscc) {
    Write-Host "==> Compiling installer with ISCC" -ForegroundColor Cyan
    & $iscc "/Q" "/DAppVersion=$Version" (Join-Path $ScriptDir 'wbsh.iss')
    if ($LASTEXITCODE -ne 0) { throw "ISCC failed (exit $LASTEXITCODE)." }
    $setupExe = Join-Path $outDir "wbsh-setup-$Platform.exe"
    if (Test-Path $setupExe) {
        Write-Host "    installer -> $setupExe" -ForegroundColor Green
    } else {
        Write-Warning "Expected installer not found at $setupExe."
    }
}
