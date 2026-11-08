# Usage: scripts/package.ps1 [-Preset debug|release] [-OutputDirectory dist] [-NoInstaller]
#
# Builds, then creates in the output folder:
#   LithiumDrums-<version>-windows-setup.exe   the graphical installer (needs Inno Setup 6 or 7)
#   LithiumDrums-<version>-windows-x64.zip     the same files, for installing by hand
[CmdletBinding()]
param(
    [ValidateSet("debug", "release")]
    [string]$Preset = "release",
    [string]$OutputDirectory = "dist",
    [switch]$NoInstaller
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$Version = (Select-String -Path (Join-Path $Root "CMakeLists.txt") `
    -Pattern '^project\(LithiumDrums VERSION ([^ )]+)' | Select-Object -First 1).Matches.Groups[1].Value
if ([string]::IsNullOrWhiteSpace($Version)) {
    throw "Could not determine the project version from CMakeLists.txt"
}

& (Join-Path $Root "scripts/build.ps1") -Preset $Preset

$configuration = if ($Preset -eq "release") { "Release" } else { "Debug" }
$Artifacts = Join-Path $Root "build\$Preset\LithiumDrums_artefacts\$configuration"
if (-not (Test-Path $Artifacts -PathType Container)) {
    throw "Build artifacts not found: $Artifacts"
}

$PackageName = "LithiumDrums-$Version-windows-$env:PROCESSOR_ARCHITECTURE".ToLowerInvariant()
$Output = Join-Path $Root $OutputDirectory
$Staging = Join-Path $Output $PackageName
$Archive = "$Staging.zip"
Remove-Item $Staging -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item $Archive -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path `
    "$Staging\plugins", "$Staging\standalone", "$Staging\tools", "$Staging\presets" | Out-Null

function Copy-Required([string]$Source, [string]$Destination) {
    if (-not (Test-Path $Source)) {
        throw "Expected build output not found: $Source"
    }
    Copy-Item $Source $Destination -Recurse -Force
}

Copy-Required "$Artifacts\VST3\Lithium Drums.vst3" "$Staging\plugins"
Copy-Required "$Artifacts\CLAP\Lithium Drums.clap" "$Staging\plugins"
Copy-Required "$Artifacts\Standalone\Lithium Drums.exe" "$Staging\standalone"
Copy-Required "$Root\assets\presets\*" "$Staging\presets"
Copy-Required "$Root\README.md" $Staging
Copy-Required "$Root\LICENSE" $Staging

$Importer = Join-Path $Root "build\$Preset\LithiumImport_artefacts\$configuration\LithiumImport.exe"
if (Test-Path $Importer) {
    Copy-Required $Importer "$Staging\tools"
}

# The notices are built from the licence files of the fetched dependencies.
$Python = if (Get-Command python3 -ErrorAction SilentlyContinue) { "python3" } else { "python" }
& $Python -I (Join-Path $Root "scripts\third-party-notices.py") (Join-Path $Root "build\_deps") "$Staging\THIRD-PARTY-NOTICES.txt"
if ($LASTEXITCODE -ne 0) { throw "Could not write the third-party notices" }

New-Item -ItemType Directory -Force -Path $Output | Out-Null
Compress-Archive -Path $Staging -DestinationPath $Archive
Write-Host "Created $Archive"

if (-not $NoInstaller) {
    # ISCC on the PATH, or a default install of Inno Setup 7 or 6.
    $Candidates = @((Get-Command iscc -ErrorAction SilentlyContinue).Source,
        "$env:ProgramFiles\Inno Setup 7\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 7\ISCC.exe",
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe") | Where-Object { $_ -and (Test-Path $_) }
    $IsccPath = $Candidates | Select-Object -First 1
    if (-not $IsccPath) {
        throw "Inno Setup not found (winget install JRSoftware.InnoSetup), or pass -NoInstaller"
    }
    & $IsccPath "/DAppVersion=$Version" "/DStaging=$Staging" "/DOutputDir=$Output" `
        (Join-Path $Root "packaging\windows\LithiumDrums.iss")
    if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed" }
}

Remove-Item $Staging -Recurse -Force
