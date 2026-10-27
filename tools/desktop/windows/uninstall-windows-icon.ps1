# Removes the Rhea (.rhea and .rh) file icon association from the current Windows user (HKCU).
param(
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
$Keys = @(
    "HKCU:\Software\Classes\.rhea",
    "HKCU:\Software\Classes\.rh",
    "HKCU:\Software\Classes\Rhea.SourceFile"
)

if ($DryRun) {
    Write-Output "[DRY-RUN] Would remove HKCU:\Software\Classes\.rhea, .rh, and Rhea.SourceFile"
    exit 0
}

foreach ($Key in $Keys) {
    if (Test-Path $Key) {
        Remove-Item -Path $Key -Recurse -Force
    }
}

Write-Output "[OK] Unregistered Rhea file icon associations from HKCU:\Software\Classes"
