# Copies the mod's scripts (mod\) into the Mod Organizer mod folder. The plugin DLL is built and
# installed by plugin\build.ps1; the smoke archive by smoke\build_smoke.py.
# Usage: powershell -ExecutionPolicy Bypass -File install.ps1 [-ModDir <folder>]
param([string]$ModDir = "F:\Cyberpunk 2077\mods\Cyberpunk Wind Framework")
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $here "mod"
foreach ($f in Get-ChildItem $src -Recurse -File) {
    $rel = $f.FullName.Substring($src.Length + 1)
    $dst = Join-Path $ModDir $rel
    New-Item -ItemType Directory -Force (Split-Path -Parent $dst) | Out-Null
    Copy-Item $f.FullName $dst -Force
    Write-Host "installed $rel"
}
