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
    # PowerShell doesn't stop when a program fails, so check each one: a
    # failed build must stop here, with the compiler's error the last thing
    # in the log, not carry on to packaging.
    cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed (exit code $LASTEXITCODE)" }
    cmake --build --preset $Preset @BuildArguments
    if ($LASTEXITCODE -ne 0) { throw "Build failed (exit code $LASTEXITCODE); the compiler's error is above" }
} finally {
    Pop-Location
}

Write-Host ""
Write-Host "Built into build\$Preset\LithiumDrums_artefacts\"
