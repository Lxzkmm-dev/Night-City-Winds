# One-off: swaps the live mod from the Cyberpunk Wind Framework names to the Night City Winds ones
# as soon as the game exits (it holds the old DLL and archive open). Removes the old plugin folder,
# the old scripts folder and the old archive, then installs the new plugin, scripts and archive.
# Writes the outcome to tools\migrate_0_5_1.log.
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$mod = "F:\Cyberpunk 2077\mods\Cyberpunk Wind Framework"
$log = Join-Path $root "tools\migrate_0_5_1.log"
"$(Get-Date -Format s) waiting for the game to exit" | Out-File $log -Encoding utf8
Wait-Process -Name Cyberpunk2077 -ErrorAction SilentlyContinue
Start-Sleep -Seconds 3
try {
    foreach ($old in @("$mod\red4ext\plugins\CyberpunkWindFramework", "$mod\r6\scripts\CyberpunkWindFramework",
                       "$mod\archive\pc\mod\!!!!!CyberpunkWindFramework_SmokeWind.archive")) {
        if (Test-Path -LiteralPath $old) { Remove-Item -LiteralPath $old -Recurse -Force; "removed $old" | Out-File $log -Append -Encoding utf8 }
    }
    New-Item -ItemType Directory -Force "$mod\red4ext\plugins\NightCityWinds", "$mod\archive\pc\mod" | Out-Null
    Copy-Item -LiteralPath (Join-Path $root "plugin\build\NightCityWinds.dll") "$mod\red4ext\plugins\NightCityWinds\NightCityWinds.dll" -Force
    "installed plugin" | Out-File $log -Append -Encoding utf8
    $src = Join-Path $root "mod"
    foreach ($f in Get-ChildItem $src -Recurse -File) {
        $rel = $f.FullName.Substring($src.Length + 1)
        $dst = Join-Path $mod $rel
        New-Item -ItemType Directory -Force (Split-Path -Parent $dst) | Out-Null
        Copy-Item -LiteralPath $f.FullName $dst -Force
    }
    "installed scripts" | Out-File $log -Append -Encoding utf8
    $arc = Join-Path $root "smoke\work\!!!!!NightCityWinds_SmokeWind.archive"
    if (Test-Path -LiteralPath $arc) {
        Copy-Item -LiteralPath $arc "$mod\archive\pc\mod\!!!!!NightCityWinds_SmokeWind.archive" -Force
        "installed archive $((Get-Item -LiteralPath $arc).Length) bytes" | Out-File $log -Append -Encoding utf8
    } else {
        "NO ARCHIVE BUILT at $arc" | Out-File $log -Append -Encoding utf8
    }
    "$(Get-Date -Format s) done" | Out-File $log -Append -Encoding utf8
} catch {
    "$(Get-Date -Format s) FAILED: $($_.Exception.Message)" | Out-File $log -Append -Encoding utf8
}
