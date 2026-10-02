# Builds the plugin (Release) with VS 18's MSVC + CMake + Ninja and copies it into the MO2 mod folder.
# Usage: powershell -ExecutionPolicy Bypass -File build.ps1 [-NoInstall]
param([switch]$NoInstall)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$vs = "C:\Program Files\Microsoft Visual Studio\18\Community"
$cmake = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
$build = Join-Path $here "build"

# import the x64 developer environment from vcvars64.bat
$envDump = cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && set"
foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2]) }
}

& $cmake -S $here -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw "configure failed" }
& $cmake --build $build
if ($LASTEXITCODE -ne 0) { throw "build failed" }

if (-not $NoInstall) {
    $dst = "F:\Cyberpunk 2077\mods\Cyberpunk Wind Framework\red4ext\plugins\CyberpunkWindFramework"
    New-Item -ItemType Directory -Force $dst | Out-Null
    $dll = Join-Path $dst "CyberpunkWindFramework.dll"
    # a running game holds the DLL: a loaded DLL can still be renamed, so move it aside
    try { Remove-Item "$dll.old" -Force -ErrorAction Stop } catch {}
    if (Test-Path $dll) {
        try { Remove-Item $dll -Force -ErrorAction Stop } catch { Rename-Item $dll "CyberpunkWindFramework.dll.old" -Force }
    }
    Copy-Item (Join-Path $build "CyberpunkWindFramework.dll") $dst -Force
    Write-Host "installed to $dst"
}
