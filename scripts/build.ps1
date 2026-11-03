# Usage: scripts/build.ps1 [-Preset debug|release] [additional cmake --build arguments]
[CmdletBinding()]
param(
    [ValidateSet("debug", "release")]
    [string]$Preset = "debug",
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$BuildArguments
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Push-Location $Root
try {
    cmake --preset $Preset
    cmake --build --preset $Preset @BuildArguments
} finally {
    Pop-Location
}

Write-Host ""
Write-Host "Built into build\$Preset\LithiumDrums_artefacts\"
