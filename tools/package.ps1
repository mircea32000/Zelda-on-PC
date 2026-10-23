# Builds the Windows installer: stages the files that get installed and compiles installer\ZeldaOnPC.iss.
#
# The staged folder (build\stage) is also a complete portable copy: add a settings.ini with the game folder
# ([Game] Path=...) or a "game" folder next to ZeldaOnPC.exe and it runs from anywhere.
#
# Requirements: Inno Setup 6.3 or newer (https://jrsoftware.org/isinfo.php) and a finished build (or pass -Build).
# Usage: powershell -ExecutionPolicy Bypass -File tools\package.ps1 [-Version 1.3.1] [-Build] [-StageOnly]
param([string]$Version = "1.3.7", [switch]$Build, [switch]$StageOnly)
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "bin"
$stage = Join-Path $root "build\stage"

if ($Build) {
    & cmd /c "`"$root\tools\build.cmd`" all release"
    if ($LASTEXITCODE -ne 0) { throw "The build failed." }
}
$exe = Join-Path $bin "ZeldaOnPC_release.exe"
if (-not (Test-Path $exe)) { throw "bin\ZeldaOnPC_release.exe was not found. Run tools\build.cmd first, or pass -Build." }

# community graphic packs (not stored in the repository)
$packs = Join-Path $bin "resources\pcedition\graphicPacks"
if (-not (Test-Path (Join-Path $packs "BreathOfTheWild\Graphics\rules.txt")) -or -not (Test-Path (Join-Path $packs "BreathOfTheWild\Cheats\InfiniteArrows\rules.txt"))) {
    & (Join-Path $PSScriptRoot "get_graphic_packs.ps1")
}

# ---- stage: exactly the files that are installed (nothing written at run time, no game data) ----
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
function Stage([string]$Source, [string]$Target) {
    $dest = Join-Path $stage $Target
    New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
    Copy-Item -Recurse -Force $Source $dest
}
Stage $exe "ZeldaOnPC.exe"
# AMD FidelityFX runtime (FSR 3 frame generation): the prebuilt, signed DLL of the FidelityFX SDK
Stage (Join-Path $root "dependencies\FidelityFX-SDK\PrebuiltSignedDLL\amd_fidelityfx_vk.dll") "amd_fidelityfx_vk.dll"
# game profiles of the three regions
foreach ($profile in Get-ChildItem (Join-Path $bin "gameProfiles\default") | Where-Object { $_.Name -match "^00050000101c9[345]00\.ini$" }) {
    Stage $profile.FullName ("gameProfiles\default\" + $profile.Name.ToLower())
}
# replacement system fonts of Cemu, key prompt font, community graphic packs
Stage (Join-Path $bin "resources\sharedFonts") "resources\sharedFonts"
Stage (Join-Path $bin "resources\pcedition\External_00.bffnt") "resources\pcedition\External_00.bffnt"
Stage $packs "resources\pcedition\graphicPacks"
# licences
Stage (Join-Path $root "LICENSE.txt") "LICENSE.txt"
Stage (Join-Path $root "installer\THIRD-PARTY-NOTICES.txt") "THIRD-PARTY-NOTICES.txt"
Stage (Join-Path $root "dependencies\FidelityFX-SDK\LICENSE.txt") "licenses\FidelityFX-SDK.txt"
Stage (Join-Path $root "dependencies\imgui\LICENSE.txt") "licenses\imgui.txt"

$size = (Get-ChildItem -Recurse -File $stage | Measure-Object -Sum Length).Sum
Write-Output ("Staged {0} files, {1:N1} MB in {2}" -f (Get-ChildItem -Recurse -File $stage).Count, ($size / 1MB), $stage)
if ($StageOnly) { exit 0 }

# ---- installer ----
$iscc = $null
$onPath = Get-Command iscc.exe -ErrorAction SilentlyContinue
if ($onPath) { $iscc = $onPath.Source }
foreach ($dir in @("$env:LOCALAPPDATA\Programs\Inno Setup 6", "${env:ProgramFiles(x86)}\Inno Setup 6", "$env:ProgramFiles\Inno Setup 6")) {
    if (-not $iscc -and (Test-Path (Join-Path $dir "ISCC.exe"))) { $iscc = Join-Path $dir "ISCC.exe" }
}
if (-not $iscc) { throw "Inno Setup 6 (ISCC.exe) was not found. Install it from https://jrsoftware.org/isinfo.php" }

& $iscc "/Q" "/DAppVersion=$Version" "/DStageDir=$stage" (Join-Path $root "installer\ZeldaOnPC.iss")
if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed." }
$setup = Join-Path $root "installer\output\ZeldaOnPC-Setup-$Version.exe"
Write-Output ("Installer: {0} ({1:N1} MB)" -f $setup, ((Get-Item $setup).Length / 1MB))
