# Usage: scripts/package.ps1 [-Preset debug|release] [-OutputDirectory dist]
[CmdletBinding()]
param(
    [ValidateSet("debug", "release")]
    [string]$Preset = "release",
    [string]$OutputDirectory = "dist"
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
    "$Staging\plugins", "$Staging\standalone", "$Staging\tools", `
    "$Staging\share\lithium-drums\presets" | Out-Null

function Copy-Required([string]$Source, [string]$Destination) {
    if (-not (Test-Path $Source)) {
        throw "Expected build output not found: $Source"
    }
    Copy-Item $Source $Destination -Recurse -Force
}

Copy-Required "$Artifacts\VST3\Lithium Drums.vst3" "$Staging\plugins"
Copy-Required "$Artifacts\CLAP\Lithium Drums.clap" "$Staging\plugins"
Copy-Required "$Artifacts\Standalone\Lithium Drums.exe" "$Staging\standalone"
Copy-Required "$Root\assets\presets\*" "$Staging\share\lithium-drums\presets"
Copy-Required "$Root\README.md" $Staging

$Importer = Join-Path $Root "build\$Preset\LithiumImport_artefacts\$configuration\LithiumImport.exe"
if (Test-Path $Importer) {
    Copy-Required $Importer "$Staging\tools"
}

New-Item -ItemType Directory -Force -Path $Output | Out-Null
Compress-Archive -Path $Staging -DestinationPath $Archive
Remove-Item $Staging -Recurse -Force
Write-Host "Created $Archive"
