# installer/build.ps1 -- produce wbsh installer .exe + portable .zip.
#
# Steps:
#   1. Locate MSBuild via vswhere.
#   2. Build wbsh.vcxproj in Release|x64.
#   3. Build wbshterm\wbshterm.vcxproj in the same configuration.
#   4. Stage each payload: wbsh on its own, and wbshterm with wbsh beside it.
#   5. Compile both .iss files, emitting wbsh-setup-x64.exe and
#      wbshterm-setup-x64.exe into output\.
#   6. Zip each staged tree into output\<product>-<ver>-portable-x64.zip.
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
    $propsPath = Join-Path $RepoRoot 'version.props'
    $propsXml  = [xml](Get-Content $propsPath)
    $vMajor = $null; $vMinor = $null; $vPatch = $null
    foreach ($pg in $propsXml.Project.PropertyGroup) {
        if ($pg.WbshVersionMajor) { $vMajor = $pg.WbshVersionMajor.Trim() }
        if ($pg.WbshVersionMinor) { $vMinor = $pg.WbshVersionMinor.Trim() }
        if ($pg.WbshVersionPatch) { $vPatch = $pg.WbshVersionPatch.Trim() }
    }
    if ($null -eq $vMajor -or $null -eq $vMinor -or $null -eq $vPatch) {
        throw "Could not locate WbshVersionMajor/Minor/Patch in $propsPath. Either set them there or pass -Version."
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

Write-Host "==> Building wbshterm $Version ($Configuration|$Platform)" -ForegroundColor Cyan
& $msbuild (Join-Path $RepoRoot 'wbshterm\wbshterm.vcxproj') `
    -nologo "-p:Configuration=$Configuration" "-p:Platform=$Platform" `
    "-p:WbshVersion=$Version" `
    "-p:WbshVersionMajor=$VerMajor" `
    "-p:WbshVersionMinor=$VerMinor" `
    "-p:WbshVersionPatch=$VerPatch" `
    -v:minimal
if ($LASTEXITCODE -ne 0) { throw "MSBuild failed for wbshterm (exit $LASTEXITCODE)." }

$termPath = Join-Path $RepoRoot "$Platform\$Configuration\wbshterm.exe"
if (-not (Test-Path $termPath)) { throw "Build did not produce $termPath." }

# --- Stage payload --------------------------------------------------------
Write-Host "==> Staging payload" -ForegroundColor Cyan
$stage = Join-Path $ScriptDir 'stage'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage | Out-Null

Copy-Item $exePath                                     $stage
Copy-Item (Join-Path $ScriptDir 'wbsh-here.cmd')       $stage

# Copy VC++ runtime DLLs app-local. Required because the projects link the
# dynamic CRT (/MD); without these the binaries fail to start on machines
# that don't already have the matching VC redistributable.
function Copy-CrtDlls {
    param([string]$Destination)

    $redistRoot = Join-Path $vsRoot 'VC\Redist\MSVC'
    if (-not (Test-Path $redistRoot)) {
        Write-Warning "VC redist root not found: $redistRoot"
        return
    }

    $latestVer = Get-ChildItem $redistRoot -Directory |
        Where-Object { $_.Name -match '^\d' } |
        Sort-Object Name -Descending | Select-Object -First 1
    if (-not $latestVer) { return }

    $crtDir = Join-Path $latestVer.FullName "$Platform\Microsoft.VC143.CRT"
    if (-not (Test-Path $crtDir)) {
        Write-Warning "VC redist CRT directory not found: $crtDir"
        return
    }

    foreach ($dll in @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
        $src = Join-Path $crtDir $dll
        if (Test-Path $src) { Copy-Item $src $Destination }
        else { Write-Warning "Missing runtime DLL: $src" }
    }
}

Copy-CrtDlls -Destination $stage

# --- Portable ZIP ---------------------------------------------------------
Write-Host "==> Producing portable ZIP" -ForegroundColor Cyan
$outDir = Join-Path $ScriptDir 'output'
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$zipPath = Join-Path $outDir "wbsh-$Version-portable-$Platform.zip"
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zipPath
Write-Host "    portable -> $zipPath" -ForegroundColor Green

# --- Stage the terminal ----------------------------------------------------
# wbsh.exe ships with it: wbshterm looks for the shell next to itself, so a
# wbshterm install works without a separate wbsh install.
Write-Host "==> Staging wbshterm payload" -ForegroundColor Cyan
$termStage = Join-Path $ScriptDir 'stage-wbshterm'
if (Test-Path $termStage) { Remove-Item $termStage -Recurse -Force }
New-Item -ItemType Directory -Path $termStage | Out-Null

Copy-Item $termPath                                        $termStage
Copy-Item $exePath                                         $termStage
Copy-Item (Join-Path $ScriptDir 'wbshterm-here.cmd')       $termStage
Copy-CrtDlls -Destination $termStage

$termZipPath = Join-Path $outDir "wbshterm-$Version-portable-$Platform.zip"
if (Test-Path $termZipPath) { Remove-Item $termZipPath -Force }
Compress-Archive -Path (Join-Path $termStage '*') -DestinationPath $termZipPath
Write-Host "    portable -> $termZipPath" -ForegroundColor Green

# --- Installers ------------------------------------------------------------
if ($iscc) {
    foreach ($script in @('wbsh.iss', 'wbshterm.iss')) {
        Write-Host "==> Compiling $script with ISCC" -ForegroundColor Cyan
        & $iscc "/Q" "/DAppVersion=$Version" (Join-Path $ScriptDir $script)
        if ($LASTEXITCODE -ne 0) { throw "ISCC failed for $script (exit $LASTEXITCODE)." }
    }

    foreach ($name in @("wbsh-setup-$Platform.exe", "wbshterm-setup-$Platform.exe")) {
        $setupExe = Join-Path $outDir $name
        if (Test-Path $setupExe) {
            Write-Host "    installer -> $setupExe" -ForegroundColor Green
        } else {
            Write-Warning "Expected installer not found at $setupExe."
        }
    }
}
