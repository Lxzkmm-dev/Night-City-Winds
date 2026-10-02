# Install the built smoke archive as soon as the game exits (the game holds the installed one open).
# Writes the outcome to install_when_closed.log next to this script.
$built = Join-Path $PSScriptRoot "work\!!!!!NightCityWinds_SmokeWind.archive"
$dest = "F:\Cyberpunk 2077\mods\Cyberpunk Wind Framework\archive\pc\mod\!!!!!NightCityWinds_SmokeWind.archive"
$log = Join-Path $PSScriptRoot "install_when_closed.log"
"$(Get-Date -Format s) waiting for the game to exit" | Out-File $log -Encoding utf8
Wait-Process -Name Cyberpunk2077 -ErrorAction SilentlyContinue
for ($i = 0; $i -lt 20; $i++) {
    Start-Sleep -Seconds 2
    try {
        Copy-Item -LiteralPath $built -Destination $dest -Force -ErrorAction Stop
        "$(Get-Date -Format s) installed $((Get-Item -LiteralPath $dest).Length) bytes" | Out-File $log -Append -Encoding utf8
        exit 0
    } catch {
        "$(Get-Date -Format s) retry: $($_.Exception.Message)" | Out-File $log -Append -Encoding utf8
    }
}
"$(Get-Date -Format s) gave up" | Out-File $log -Append -Encoding utf8
