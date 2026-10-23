Zelda on PC for Linux (experimental)
====================================

This is a portable build: extract the folder anywhere and run ./ZeldaOnPC from it. Settings, saves, caches and the log
are kept next to the executable (data/, settings.ini).

Your game
---------
Zelda on PC runs your own dump of The Legend of Zelda: Breath of the Wild for Wii U (base game, update 1.5.0 and the
DLC), as folders with code / content / meta, as a .wua archive, or the base game as a .wud / .wux disc image. Either
put it into a folder named "game" next to ZeldaOnPC, or write its location into settings.ini next to ZeldaOnPC:

    [Game]
    Path=/path/to/your/dump

An existing Cemu save (~/.local/share/Cemu or the Flatpak's ~/.var/app/info.cemu.Cemu/data/Cemu) is copied once on
the first start; it is never moved or changed.

Notes
-----
- Built on Ubuntu 24.04 (x86_64). It needs a Vulkan driver and GTK 3; older distributions may lack a recent enough
  C library.
- AMD FSR 3 frame generation needs libamd_fidelityfx_vk.so, which AMD does not ship for Linux, so frame generation is
  not available here.
- If the game cannot start, the reason is printed in the terminal: start ZeldaOnPC from a terminal to read it.
