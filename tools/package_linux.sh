#!/bin/bash
# Stages the Linux build like the Windows installer (tools/package.ps1) and packs it into a portable .tar.gz:
# the executable, the game profiles of the three regions, Cemu's replacement system fonts, the key prompt font, the
# community graphic packs (fetched with their checksum, not stored in the repository) and the licences. No game data.
# Usage: tools/package_linux.sh <version>   (after cmake --build; writes build/ZeldaOnPC-<version>-linux-x86_64.tar.gz)
set -euo pipefail
version="$1"
root="$(cd "$(dirname "$0")/.." && pwd)"
bin="$root/bin"
name="ZeldaOnPC-$version-linux-x86_64"
stage="$root/build/stage-linux/ZeldaOnPC"

# community graphic packs, the same release and checksum as tools/get_graphic_packs.ps1
packs_version=987
packs_sha256="fff8df5d8122b18871c0485fe266873644ed5254c15ede727bad1ef5f383951d"
zip="$root/build/downloads/graphicPacks$packs_version.zip"
mkdir -p "$(dirname "$zip")"
if [ ! -f "$zip" ]; then
	curl -sSfL -o "$zip.part" "https://github.com/cemu-project/cemu_graphic_packs/releases/download/Github$packs_version/graphicPacks$packs_version.zip"
	mv "$zip.part" "$zip"
fi
echo "$packs_sha256  $zip" | sha256sum -c -

rm -rf "$root/build/stage-linux"
mkdir -p "$stage/gameProfiles/default" "$stage/resources/pcedition/graphicPacks" "$stage/licenses"
install -m 755 "$bin/ZeldaOnPC_release" "$stage/ZeldaOnPC"
for region in 3 4 5; do
	src="$(find "$bin/gameProfiles/default" -maxdepth 1 -iname "00050000101c9${region}00.ini" | head -n 1)"
	cp "$src" "$stage/gameProfiles/default/00050000101c9${region}00.ini"
done
cp -r "$bin/resources/sharedFonts" "$stage/resources/sharedFonts"
cp "$bin/resources/pcedition/External_00.bffnt" "$stage/resources/pcedition/External_00.bffnt"
unzip -q "$zip" "BreathOfTheWild/*" -d "$stage/resources/pcedition/graphicPacks"
cp "$root/LICENSE.txt" "$stage/LICENSE.txt"
cp "$root/installer/THIRD-PARTY-NOTICES.txt" "$stage/THIRD-PARTY-NOTICES.txt"
cp "$root/dependencies/FidelityFX-SDK/LICENSE.txt" "$stage/licenses/FidelityFX-SDK.txt"
cp "$root/dependencies/imgui/LICENSE.txt" "$stage/licenses/imgui.txt"
cp "$root/dist/linux/README-Linux.txt" "$stage/README-Linux.txt"

tar -C "$root/build/stage-linux" -czf "$root/build/$name.tar.gz" ZeldaOnPC
echo "Staged $(find "$stage" -type f | wc -l) files: $root/build/$name.tar.gz ($(du -h "$root/build/$name.tar.gz" | cut -f1))"
