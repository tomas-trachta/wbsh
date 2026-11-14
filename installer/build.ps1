# installer/build.ps1 -- produce wbsh installer .exe + portable .zip.
#
# Steps:
#   1. Locate MSBuild via vswhere.
#   2. Build wbsh\wbsh.vcxproj in Release|<platform> (x64 or ARM64).
#   3. Build wbshterm\wbshterm.vcxproj in the same configuration.
#   4. Stage each payload: wbsh on its own, and wbshterm with wbsh beside it.
#   5. Compile both .iss files, emitting wbsh-setup-<arch>.exe and
#      wbshterm-setup-<arch>.exe into output\.
#   6. Zip each staged tree into output\<product>-<ver>-portable-<arch>.zip.
#   7. Zip the SDK (header, property sheet, samples, template, docs,
#      symbols) into output\wbsh-sdk-<ver>.zip.
#   8. Zip the agent skill (tools/agents/skill plus the command list and
#      --agent-info of the shell just built) into output\wbsh-skill-<ver>.zip.
# Steps 7 and 8 belong to the x64 run: the SDK is the same for every
# architecture, and the skill runs the shell it was built from, which an
# x64 machine cannot do for an ARM64 build. A release runs this once per
# platform into the same output\.
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [ValidateSet('x64', 'ARM64')]
    [string]$Platform      = 'x64',
    # Defaults to the WbshVersion property in version.props (the single
    # source of truth). Pass -Version X.Y.Z to override for a one-off
    # build without editing the project file.
    [string]$Version       = ''
)

$ErrorActionPreference = 'Stop'
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = Resolve-Path (Join-Path $ScriptDir '..')

# File and installer names carry the architecture in lower case; x64 keeps
# the stage folders it always had.
$Arch        = $Platform.ToLowerInvariant()
$StageSuffix = if ($Arch -eq 'x64') { '' } else { "-$Arch" }

# Read the version from version.props if the caller didn't override.
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

# The SDK ships with both products: without it beside the exe, a util
# dropped in plugins has nothing to load against.
& $msbuild (Join-Path $RepoRoot 'sdk\wbshsdk.vcxproj') `
    -nologo "-p:Configuration=$Configuration" "-p:Platform=$Platform" `
    "-p:WbshVersion=$Version" `
    "-p:WbshVersionMajor=$VerMajor" `
    "-p:WbshVersionMinor=$VerMinor" `
    "-p:WbshVersionPatch=$VerPatch" `
    -v:minimal
if ($LASTEXITCODE -ne 0) { throw "MSBuild failed for wbshsdk (exit $LASTEXITCODE)." }

$sdkPath = Join-Path $RepoRoot "$Platform\$Configuration\wbshsdk.dll"
if (-not (Test-Path $sdkPath)) { throw "Build did not produce $sdkPath." }

& $msbuild (Join-Path $RepoRoot 'wbsh\wbsh.vcxproj') `
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
$stage = Join-Path $ScriptDir "stage$StageSuffix"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage | Out-Null

Copy-Item $exePath                                     $stage
Copy-Item (Join-Path $ScriptDir 'wbsh-here.cmd')       $stage
Copy-Item $sdkPath                                     $stage

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

# Microsoft's current ConPTY (wbshterm\conpty\fetch.ps1 pins the version),
# for the pseudoconsole `script` and `unbuffer` start their command on.
# wbsh loads it from its own directory and falls back to the one built
# into Windows when the files are missing.
function Copy-BundledConpty {
    param([string]$Destination)

    $conptyDir = Join-Path $RepoRoot 'wbshterm\conpty'
    if ($Arch -ne 'x64') { $conptyDir = Join-Path $conptyDir $Arch }

    foreach ($name in @('conpty.dll', 'OpenConsole.exe')) {
        $src = Join-Path $conptyDir $name
        if (Test-Path $src) { Copy-Item $src $Destination }
        else { Write-Warning "Bundled ConPTY file missing: $src (run wbshterm\conpty\fetch.ps1)" }
    }
}

Copy-BundledConpty -Destination $stage

# --- Portable ZIP ---------------------------------------------------------
Write-Host "==> Producing portable ZIP" -ForegroundColor Cyan
$outDir = Join-Path $ScriptDir 'output'
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$zipPath = Join-Path $outDir "wbsh-$Version-portable-$Arch.zip"
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zipPath
Write-Host "    portable -> $zipPath" -ForegroundColor Green

function Write-SharedZips {
    # --- The SDK zip -----------------------------------------------------------
    # What a third-party developer needs and nothing they do not: the header,
    # the property sheet, the samples and the template as source, the docs,
    # and the SDK's symbols so a crash inside a util has a readable stack.
    # No import library: a util is not linked against anything.
    Write-Host "==> Staging the SDK" -ForegroundColor Cyan
    $sdkStage = Join-Path $ScriptDir 'stage-sdk'
    if (Test-Path $sdkStage) { Remove-Item $sdkStage -Recurse -Force }
    New-Item -ItemType Directory -Path $sdkStage | Out-Null

    $sdkRoot = Join-Path $RepoRoot 'sdk'
    Copy-Item (Join-Path $sdkRoot 'include')        (Join-Path $sdkStage 'include')  -Recurse
    Copy-Item (Join-Path $sdkRoot 'template')       (Join-Path $sdkStage 'template') -Recurse
    Copy-Item (Join-Path $sdkRoot 'wbshutil.props') $sdkStage
    Copy-Item (Join-Path $sdkRoot 'README.md')      $sdkStage
    Copy-Item (Join-Path $sdkRoot 'CHANGELOG.md')   $sdkStage

    $sampleStage = Join-Path $sdkStage 'samples'
    New-Item -ItemType Directory -Path $sampleStage | Out-Null
    foreach ($sample in Get-ChildItem (Join-Path $sdkRoot 'samples') -Directory) {
        $target = Join-Path $sampleStage $sample.Name
        New-Item -ItemType Directory -Path $target | Out-Null
        Get-ChildItem $sample.FullName -File | Copy-Item -Destination $target
    }

    foreach ($stray in Get-ChildItem $sdkStage -Recurse -Directory -Include 'bin', 'build') {
        Remove-Item $stray.FullName -Recurse -Force -ErrorAction SilentlyContinue
    }

    $sdkPdb = Join-Path $RepoRoot "$Platform\$Configuration\wbshsdk.pdb"
    if (Test-Path $sdkPdb) { Copy-Item $sdkPdb $sdkStage }
    else { Write-Warning "SDK symbols not found at $sdkPdb; the SDK zip ships without them." }

    $sdkZipPath = Join-Path $outDir "wbsh-sdk-$Version.zip"
    if (Test-Path $sdkZipPath) { Remove-Item $sdkZipPath -Force }
    Compress-Archive -Path (Join-Path $sdkStage '*') -DestinationPath $sdkZipPath
    Write-Host "    sdk      -> $sdkZipPath" -ForegroundColor Green

    # --- The agent skill zip ---------------------------------------------------
    # SKILL.md plus two files generated from the shell just built, so the
    # command list and the brief can never drift from the release they ship with.
    Write-Host "==> Producing the agent skill" -ForegroundColor Cyan
    $skillStage = Join-Path $ScriptDir 'stage-skill'
    if (Test-Path $skillStage) { Remove-Item $skillStage -Recurse -Force }
    $skillDir = Join-Path $skillStage 'wbsh'
    New-Item -ItemType Directory -Path $skillDir | Out-Null

    Copy-Item (Join-Path $RepoRoot 'tools\agents\skill\*') $skillDir -Recurse
    & $exePath --agent-info | Set-Content -Path (Join-Path $skillDir 'agent-info.txt') -Encoding utf8
    & $exePath -c 'compgen -b | grep -v ^__ | sort' |
        Set-Content -Path (Join-Path $skillDir 'commands.txt') -Encoding utf8
    if ($LASTEXITCODE -ne 0) { throw "wbsh could not list its commands (exit $LASTEXITCODE)." }

    $skillZipPath = Join-Path $outDir "wbsh-skill-$Version.zip"
    if (Test-Path $skillZipPath) { Remove-Item $skillZipPath -Force }
    Compress-Archive -Path (Join-Path $skillStage '*') -DestinationPath $skillZipPath
    Write-Host "    skill    -> $skillZipPath" -ForegroundColor Green
}

if ($Arch -eq 'x64') { Write-SharedZips }

# --- Stage the terminal ----------------------------------------------------
# wbsh.exe ships with it: wbshterm looks for the shell next to itself, so a
# wbshterm install works without a separate wbsh install.
Write-Host "==> Staging wbshterm payload" -ForegroundColor Cyan
$termStage = Join-Path $ScriptDir "stage-wbshterm$StageSuffix"
if (Test-Path $termStage) { Remove-Item $termStage -Recurse -Force }
New-Item -ItemType Directory -Path $termStage | Out-Null

Copy-Item $termPath                                        $termStage
Copy-Item $exePath                                         $termStage
Copy-Item (Join-Path $ScriptDir 'wbshterm-here.cmd')       $termStage
Copy-Item $sdkPath                                         $termStage
Copy-CrtDlls -Destination $termStage

# wbshterm runs its panes on the same ConPTY, loaded the same way.
Copy-BundledConpty -Destination $termStage

$termZipPath = Join-Path $outDir "wbshterm-$Version-portable-$Arch.zip"
if (Test-Path $termZipPath) { Remove-Item $termZipPath -Force }
Compress-Archive -Path (Join-Path $termStage '*') -DestinationPath $termZipPath
Write-Host "    portable -> $termZipPath" -ForegroundColor Green

# --- Installers ------------------------------------------------------------
if ($iscc) {
    foreach ($script in @('wbsh.iss', 'wbshterm.iss')) {
        Write-Host "==> Compiling $script with ISCC" -ForegroundColor Cyan
        & $iscc "/Q" "/DAppVersion=$Version" "/DArch=$Arch" (Join-Path $ScriptDir $script)
        if ($LASTEXITCODE -ne 0) { throw "ISCC failed for $script (exit $LASTEXITCODE)." }
    }

    foreach ($name in @("wbsh-setup-$Arch.exe", "wbshterm-setup-$Arch.exe")) {
        $setupExe = Join-Path $outDir $name
        if (Test-Path $setupExe) {
            Write-Host "    installer -> $setupExe" -ForegroundColor Green
        } else {
            Write-Warning "Expected installer not found at $setupExe."
        }
    }
}
