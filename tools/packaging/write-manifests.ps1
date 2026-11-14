# tools/packaging/write-manifests.ps1 -- package-manager manifests for a release.
#
# Given the release artifacts in installer\output, writes:
#   installer\output\SHA256SUMS.txt                       (uploaded with the release)
#   packaging\scoop\wbsh.json, wbshterm.json              (portable ZIPs)
#   packaging\winget\manifests\t\TomasTrachta\<id>\<ver>\ (per-user Inno installers)
#
# The release workflow runs it after build.ps1; it can be run by hand too:
#   .\tools\packaging\write-manifests.ps1 -Version 1.1.1 -Artifacts <dir with the release files>

param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$Artifacts = (Join-Path $PSScriptRoot '..\..\installer\output'),
    [string]$Repo = 'tomas-trachta/wbsh'
)

$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$releaseUrl = "https://github.com/$Repo/releases/download/v$Version"

$products = @(
    @{
        Name        = 'wbsh'
        WingetId    = 'TomasTrachta.wbsh'
        Description = 'A Bash-compatible shell for Windows: a drop-in replacement for Git Bash.'
        Long        = 'wbsh is a Bash-compatible shell written from scratch in C++ for Windows, with a real POSIX shell grammar, a readline-style line editor and bundled coreutils, in one self-contained executable.'
        ProductCode = '{F4B7E8C2-3A5D-4E1F-8B92-7D6C5A4E9F12}_is1'
        Bin         = @('wbsh.exe')
        Tags        = @('bash', 'shell', 'git-bash', 'posix', 'coreutils', 'terminal')
    },
    @{
        Name        = 'wbshterm'
        WingetId    = 'TomasTrachta.wbshterm'
        Description = 'A terminal for wbsh with themes, panes, scrollback and Lua scripting.'
        Long        = 'wbshterm hosts wbsh on a pseudoconsole: themes, a config file, scrollback and selection, tmux-style panes, shell integration and Lua scripting. It bundles wbsh.'
        ProductCode = '{6E1C9D4A-2F83-4B77-9A16-58C0E7D3B441}_is1'
        Bin         = @('wbshterm.exe', 'wbsh.exe')
        Tags        = @('terminal', 'bash', 'shell', 'conpty', 'tabs', 'panes')
    }
)

# Every architecture a release can ship, under the names each package
# manager gives it. A manifest lists the ones whose files are there, so
# older releases (x64 only) still get valid manifests.
$architectures = @(
    @{ File = 'x64';   Scoop = '64bit'; Winget = 'x64' },
    @{ File = 'arm64'; Scoop = 'arm64'; Winget = 'arm64' }
)

function Get-ShippedArchitectures([string]$Pattern) {
    $shipped = @($architectures | Where-Object {
        Test-Path (Join-Path $Artifacts ($Pattern -f $_.File))
    })
    if ($shipped.Count -eq 0) { throw "Missing release artifact: $($Pattern -f 'x64')" }
    return $shipped
}

function Get-ArtifactPath([string]$FileName) {
    $path = Join-Path $Artifacts $FileName
    if (-not (Test-Path $path)) { throw "Missing release artifact: $path" }
    return $path
}

function Get-Sha256([string]$FileName) {
    return (Get-FileHash -Algorithm SHA256 (Get-ArtifactPath $FileName)).Hash.ToLowerInvariant()
}

function Write-Utf8File([string]$Path, [string]$Text) {
    New-Item -ItemType Directory -Force -Path (Split-Path $Path) | Out-Null
    $normalized = $Text.Replace("`r`n", "`n")
    if (-not $normalized.EndsWith("`n")) { $normalized += "`n" }
    [System.IO.File]::WriteAllText($Path, $normalized, (New-Object System.Text.UTF8Encoding $false))
}

function Write-Sha256Sums {
    $lines = Get-ChildItem $Artifacts -File |
        Where-Object { $_.Extension -in '.exe', '.zip' } |
        Sort-Object Name |
        ForEach-Object { "{0}  {1}" -f (Get-Sha256 $_.Name), $_.Name }
    Write-Utf8File (Join-Path $Artifacts 'SHA256SUMS.txt') (($lines -join "`n") + "`n")
}

function Get-ScoopArchitectures($Product) {
    $current = [ordered]@{}
    $future = [ordered]@{}
    foreach ($arch in Get-ShippedArchitectures "$($Product.Name)-$Version-portable-{0}.zip") {
        $zip = "$($Product.Name)-$Version-portable-$($arch.File).zip"
        $current[$arch.Scoop] = [ordered]@{ url = "$releaseUrl/$zip"; hash = (Get-Sha256 $zip) }
        $future[$arch.Scoop] = [ordered]@{
            url = "https://github.com/$Repo/releases/download/v`$version/$($Product.Name)-`$version-portable-$($arch.File).zip"
        }
    }
    return $current, $future
}

function Write-ScoopManifest($Product) {
    $current, $future = Get-ScoopArchitectures $Product
    $manifest = [ordered]@{
        version      = $Version
        description  = $Product.Description
        homepage     = "https://github.com/$Repo"
        license      = 'MIT'
        architecture = $current
        bin          = $Product.Bin
        checkver     = 'github'
        autoupdate   = [ordered]@{
            architecture = $future
            hash         = [ordered]@{ url = '$baseurl/SHA256SUMS.txt' }
        }
    }
    $path = Join-Path $root "packaging\scoop\$($Product.Name).json"
    Write-Utf8File $path (($manifest | ConvertTo-Json -Depth 8) + "`n")
}

function Get-WingetDirectory($Product) {
    $publisher, $package = $Product.WingetId.Split('.')
    $letter = $publisher.Substring(0, 1).ToLowerInvariant()
    return Join-Path $root "packaging\winget\manifests\$letter\$publisher\$package\$Version"
}

function Write-WingetVersion($Product, [string]$Directory) {
    Write-Utf8File (Join-Path $Directory "$($Product.WingetId).yaml") @"
# yaml-language-server: `$schema=https://aka.ms/winget-manifest.version.1.6.0.schema.json
PackageIdentifier: $($Product.WingetId)
PackageVersion: $Version
DefaultLocale: en-US
ManifestType: version
ManifestVersion: 1.6.0
"@
}

function Get-WingetInstallers($Product) {
    $entries = foreach ($arch in Get-ShippedArchitectures "$($Product.Name)-setup-{0}.exe") {
        $setup = "$($Product.Name)-setup-$($arch.File).exe"
        @"
  - Architecture: $($arch.Winget)
    InstallerUrl: $releaseUrl/$setup
    InstallerSha256: $((Get-Sha256 $setup).ToUpperInvariant())
"@
    }
    return ($entries -join "`n")
}

function Write-WingetInstaller($Product, [string]$Directory) {
    Write-Utf8File (Join-Path $Directory "$($Product.WingetId).installer.yaml") @"
# yaml-language-server: `$schema=https://aka.ms/winget-manifest.installer.1.6.0.schema.json
PackageIdentifier: $($Product.WingetId)
PackageVersion: $Version
InstallerType: inno
Scope: user
InstallModes:
  - interactive
  - silent
  - silentWithProgress
UpgradeBehavior: install
ProductCode: '$($Product.ProductCode)'
Installers:
$(Get-WingetInstallers $Product)
ManifestType: installer
ManifestVersion: 1.6.0
"@
}

function Write-WingetLocale($Product, [string]$Directory) {
    $tags = ($Product.Tags | ForEach-Object { "  - $_" }) -join "`n"
    Write-Utf8File (Join-Path $Directory "$($Product.WingetId).locale.en-US.yaml") @"
# yaml-language-server: `$schema=https://aka.ms/winget-manifest.defaultLocale.1.6.0.schema.json
PackageIdentifier: $($Product.WingetId)
PackageVersion: $Version
PackageLocale: en-US
Publisher: Tomas Trachta
PublisherUrl: https://github.com/tomas-trachta
PublisherSupportUrl: https://github.com/$Repo/issues
PackageName: $($Product.Name)
PackageUrl: https://github.com/$Repo
License: MIT
LicenseUrl: https://github.com/$Repo/blob/main/LICENSE
ShortDescription: '$($Product.Description.Replace("'", "''"))'
Description: '$($Product.Long.Replace("'", "''"))'
Tags:
$tags
ReleaseNotesUrl: https://github.com/$Repo/releases/tag/v$Version
ManifestType: defaultLocale
ManifestVersion: 1.6.0
"@
}

function Write-WingetManifests($Product) {
    $directory = Get-WingetDirectory $Product
    Write-WingetVersion $Product $directory
    Write-WingetInstaller $Product $directory
    Write-WingetLocale $Product $directory
}

Write-Sha256Sums
foreach ($product in $products) {
    Write-ScoopManifest $product
    Write-WingetManifests $product
}

Write-Host "Manifests for $Version written under $(Join-Path $root 'packaging')"
