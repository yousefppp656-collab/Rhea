# Registers the Rhea (.rhea and .rh) file icon (rhea.ico) for the current Windows user (HKCU).
param(
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..\..")
$IcoPath = Join-Path $RepoRoot "assets\icons\rhea.ico"

if (-not (Test-Path $IcoPath)) {
    throw "Missing Rhea icon file at $IcoPath"
}

if ($DryRun) {
    Write-Output "[DRY-RUN] Would register .rhea and .rh with DefaultIcon '$IcoPath,0'"
    exit 0
}

New-Item -Path "HKCU:\Software\Classes\.rhea" -Force | Out-Null
Set-ItemProperty -Path "HKCU:\Software\Classes\.rhea" -Name "(Default)" -Value "Rhea.SourceFile"
New-Item -Path "HKCU:\Software\Classes\.rh" -Force | Out-Null
Set-ItemProperty -Path "HKCU:\Software\Classes\.rh" -Name "(Default)" -Value "Rhea.SourceFile"

New-Item -Path "HKCU:\Software\Classes\Rhea.SourceFile" -Force | Out-Null
Set-ItemProperty -Path "HKCU:\Software\Classes\Rhea.SourceFile" -Name "(Default)" -Value "Rhea Source File"
New-Item -Path "HKCU:\Software\Classes\Rhea.SourceFile\DefaultIcon" -Force | Out-Null
Set-ItemProperty -Path "HKCU:\Software\Classes\Rhea.SourceFile\DefaultIcon" -Name "(Default)" -Value "$IcoPath,0"

Write-Output "[OK] Registered .rhea and .rh file icon ($IcoPath) in HKCU:\Software\Classes"
