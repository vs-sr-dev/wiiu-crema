# WiiU-GX2-PoC deploy: build + copy .wuhb to Cemu staging / real SD, then launch Cemu.
# Usage:
#   .\deploy.ps1 poc1            -> build all, deploy+launch poc1
#   .\deploy.ps1 poc1 -NoLaunch  -> deploy only
param(
    [string]$Poc = "poc1",
    [switch]$NoLaunch
)

$ErrorActionPreference = "Stop"
$projectRoot = $PSScriptRoot
$cemuExe = "D:\Homebrew5\WiiULBA\Cemu_2.6\Cemu.exe"

Write-Host "[GX2PoC] building..." -ForegroundColor Cyan
Push-Location $projectRoot
try {
    bash build.sh
    if ($LASTEXITCODE -ne 0) { throw "build.sh failed (exit $LASTEXITCODE)" }
} finally {
    Pop-Location
}

$wuhb = Get-ChildItem (Join-Path $projectRoot "build") -Recurse -Filter "$Poc*.wuhb" | Select-Object -First 1
if (-not $wuhb) { throw "No $Poc*.wuhb found under build/" }

$targets = @(
    "D:\cemu-sd\wiiu\apps\gx2poc\$($wuhb.Name)",
    "G:\wiiu\apps\gx2poc\$($wuhb.Name)"
)
foreach ($t in $targets) {
    $parent = Split-Path $t -Parent
    if (-not (Test-Path $parent)) {
        if ($t.StartsWith("D:") -or (Test-Path (Split-Path (Split-Path $parent -Parent) -Parent))) {
            New-Item -ItemType Directory -Force $parent | Out-Null
        } else {
            Write-Host "[deploy] skip $t (SD not inserted)" -ForegroundColor Yellow; continue
        }
    }
    Copy-Item -Force $wuhb.FullName $t
    Write-Host "[deploy] OK -> $t" -ForegroundColor Green
}

if (-not $NoLaunch) {
    Get-Process Cemu -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Process $cemuExe -ArgumentList '-g', "`"D:\cemu-sd\wiiu\apps\gx2poc\$($wuhb.Name)`""
    Write-Host "[deploy] Cemu launched with $($wuhb.Name)" -ForegroundColor Green
}
