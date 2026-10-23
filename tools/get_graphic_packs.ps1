# Fetches the community graphic packs for Breath of the Wild ("Graphics" and "FPS++", which Zelda on PC builds on, and
# the optional ones offered on its Mods page) from the official Cemu graphic pack release this version was tested with,
# and puts them next to the executable's resources.
#
# The packs are made by the Cemu community (https://github.com/cemu-project/cemu_graphic_packs, CC0-1.0). They are not
# stored in this repository; the installer ships the copy fetched here.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\get_graphic_packs.ps1 [-Destination <folder>]
param([string]$Destination = "")
$ErrorActionPreference = "Stop"

$version = 987
$sha256 = "FFF8DF5D8122B18871C0485FE266873644ED5254C15EDE727BAD1EF5F383951D"
$url = "https://github.com/cemu-project/cemu_graphic_packs/releases/download/Github$version/graphicPacks$version.zip"
$packs = @("BreathOfTheWild/")

$root = Split-Path -Parent $PSScriptRoot
if (-not $Destination) { $Destination = Join-Path $root "bin\resources\pcedition\graphicPacks" }
$cache = Join-Path $root "build\downloads"
$zip = Join-Path $cache "graphicPacks$version.zip"

New-Item -ItemType Directory -Force $cache | Out-Null
if (-not (Test-Path $zip)) {
    Write-Output "Downloading $url"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $ProgressPreference = "SilentlyContinue"
    Invoke-WebRequest -Uri $url -OutFile "$zip.part" -UseBasicParsing
    Move-Item -Force "$zip.part" $zip
}
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash
if ($hash -ne $sha256) {
    Remove-Item -Force $zip
    throw "graphicPacks$version.zip has an unexpected checksum ($hash). The download was removed; run the script again."
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($zip)
try {
    foreach ($pack in $packs) {
        $target = Join-Path $Destination ($pack -replace "/", "\")
        if (Test-Path $target) { Remove-Item -Recurse -Force $target }
        $count = 0
        foreach ($entry in $archive.Entries) {
            if (-not $entry.FullName.StartsWith($pack) -or $entry.FullName.EndsWith("/")) { continue }
            $out = Join-Path $Destination ($entry.FullName -replace "/", "\")
            New-Item -ItemType Directory -Force (Split-Path -Parent $out) | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $out, $true)
            $count++
        }
        if ($count -eq 0) { throw "$pack was not found in graphicPacks$version.zip" }
        Write-Output "$pack ($count files)"
    }
}
finally {
    $archive.Dispose()
}
Set-Content -Encoding utf8 (Join-Path $Destination "README.txt") @"
Cemu community graphic packs v$version (Breath of the Wild)
Source:  https://github.com/cemu-project/cemu_graphic_packs
License: CC0-1.0
"@
Write-Output "Graphic packs v$version are in $Destination"
