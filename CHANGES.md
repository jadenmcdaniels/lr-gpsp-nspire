# gpSP for the TI-Nspire CX and CX II: version 2.0 beta

Version 2.0 beta is the build the About screen calls r19. It runs on the TI-Nspire CX, CX CAS and CX II
under Ndless.

**This is a beta.** Everything below passes the PC tests (`tests/gate.sh`) and has run on a CX and a CX II,
but not in every combination of devices. The least proven part is Bluetooth with a USB hub (the dongle
on a hub beside a controller, a drive or a sound adapter): use the dongle alone on the OTG adapter when you
can, and report a freeze with the log files gpSP leaves in zgpSP_files.

The menus are described screen by screen in `docs/ui/r19.txt`; the Bluetooth, USB drive, USB sound and
seamless USB port details are in `docs/bluetooth.txt`, `docs/usb-drive.txt`, `docs/usb-audio.txt` and
`docs/seamless-usb.txt`.

## Menus

- One screen. Its top row has a word in each corner, SETTINGS and QUIT, and the selector between them:
  GAMES, and SAVES before it while a game is paused. Left and right move along the row; what is under a
  word shows at once; down or enter goes into it, up or esc comes back.
- GAMES is a grid of pictures, three by two, each game's newest save with its name under it. Only the
  picked game is in colour. The folders come after the games, as items too (the USB drive among them),
  so there is no folder menu. V shows a line a game, tab moves a game in the list (shift+tab sorts A to
  Z), S opens a game's saves, U the USB drive.
- Capital letters only, in one monospaced face (IBM Plex Mono) at one size. A name too long for its tile
  scrolls smoothly and comes round again.
- Every page has a path at the top ("SETTINGS / MORE / ABOUT"), so it is always clear how deep it is.
- The band at the bottom says what the picked thing is, the one or two keys it needs, what just happened,
  and the battery.
- QUIT is one press from the top row and asks nothing (the game is saved first when Save on quit is on).

## Settings

- One screen of tiles, three across, each a name over its value: VOLUME, BRIGHTNESS, SCREEN SIZE, FPS
  COUNTER, AUTOSAVE, KEYS, CONTROLLER, BLUETOOTH, LINK (with a game), CHEATS (when the game has codes)
  and MORE. Nothing scrolls.
- A setting with up to four values changes with one press of enter; a range (volume, brightness,
  autosave) goes between < > and left and right change it.
- MORE holds the rest on one page: frame skip (Auto or Off), where the save key and the autosave put a
  save (the same slot or a new one), where saves go while a USB drive is in, the link device, settings and
  keys for this game only, the LCD fix (older CX), restart, the save manager, the games manager and
  About.
- Choices that only decided speed have no tile and are set to the fast value: the flicker fix off, the
  fast compatibility mode, no top speed for fast forward. The Fit screen size is always the smoothed
  picture.
- Screen size: Original (the GBA's own pixels, with a border), Fit (the width filled, bars above and
  below), Stretch (the whole screen).
- Settings for all games are kept inside the program file itself, with a copy in zgpSP_files, so they
  travel with `_gpSP.tns`.

## Keys

- Settings > Keys lists every action with its key, beside a drawing of the calculator's keypad that
  shows what each key does. Two keys an action, key combinations, and turbo for the GBA buttons.
- Unbound actions say NO KEY. 8 5 4 6 are the arrows in a game; 7 9 1 3 do nothing unless bound.
- Hotkeys for brightness up and down, save, load, load newest, next and previous slot, the FPS counter,
  save and quit, and restart. * and / change the volume in a game.

## Cheats

- RetroArch's `.cht` files (the ones in libretro-database), named after the game, are found in the
  game's folder, in zgpSP_files, or on a USB drive (its top folder, `cheats` or `zgpSP_files`).
- Settings > Cheats lists the codes, each with an on/off switch; the file remembers which are on.
- CodeBreaker codes; up to 160 kept from a file and 20 on at once. Codes of other formats are counted
  and left out.

## Controllers

- USB controllers on the calculator's port, through an OTG adapter or a hub: Xbox 360 and Xbox One /
  Series wired pads, DualShock 4, DualSense, Switch Pro Controller, 8BitDo pads and generic HID pads.
  Up to two players.
- Plug and play: gpSP watches the port and brings up whatever is plugged in, power-cycling the port when
  the OS doesn't offer the device; nothing has to be picked first.
- Settings > Controller is one list of what is connected (USB or Bluetooth), with a sentence when the
  port needs something from the player, a restart of the port's power, and the port's details for a
  report.
- When what is plugged in draws more power than the port gives, the page says so ("Not enough power for
  what is plugged in. Use a hub with its own power, or Bluetooth.") instead of reading it as an unplug.

## Bluetooth

- A USB Bluetooth dongle on the calculator's port (through the OTG adapter, or a hub) connects Bluetooth
  controllers (DualSense, DualShock 4, Switch Pro Controller, 8BitDo, and Bluetooth LE controllers such as
  the Xbox Wireless Controller) and Bluetooth headphones. Tested: the TP-Link UB500 (2357:0604, Realtek
  RTL8761BU). CSR and Broadcom dongles need nothing more.
- A Realtek dongle needs its firmware, which gpSP uploads at every start. Take `rtl8761bu_fw.bin` and
  `rtl8761bu_config.bin` from the `rtl_bt` folder of linux-firmware
  (https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git; on a Linux PC they are in
  /lib/firmware/rtl_bt, perhaps compressed as .zst: unpack them), and put them on the calculator as
  `rtl8761bu_fw.bin.tns` and `rtl8761bu_config.bin.tns` in My Documents/zgpSP_files/bt/ (make the bt
  folder if it isn't there). They are read once when Bluetooth starts; without them the Bluetooth page
  says the firmware file is missing.
- Settings > Bluetooth lists every device nearby, as a phone does: the paired ones, then what was found,
  each with what it is (CONTROLLER, HEADPHONES, ...) and WEAK when its signal is. Enter pairs or
  connects; enter again stops a pairing or a connection (a misclick undone); del forgets. One connect at a
  time: another one asked waits its turn.
- The page says what Bluetooth is waiting for: the dongle, a hub's devices ("A hub is in: its devices take
  up to 15 s."), the dongle starting. If the dongle stops answering it is restarted, and a connect that
  the restart ended says so.
- Paired devices are remembered; the last headphones reconnect by themselves.
- Bluetooth can be switched on or off while the dongle is starting or stopping; the switch is kept and
  applied when that ends.

## Saves

- Save states in slots 0 to 999, each with a picture of the game; SAVES shows them as a grid with RESUME,
  SAVE and the game's own save first.
- A save goes to a new slot or the same one (a setting); autosave every 30 s to 30 min; starting a game
  resumes its newest state (shift+enter starts without it).
- The save manager marks, deletes, packs and unpacks states, and copies them between the calculator and
  a USB drive. Saves can be kept on a USB drive while one is in.
- States are zlib-compressed and in-game saves gzipped when packed; plain files are read too.
- Saving keeps the sound playing and the game running: the file is written in small steps between
  frames.

## USB drives

- Games can be played from a USB drive (FAT16, FAT32, exFAT): .gba, .gbz and .zip.
- Saves and states can be written to the drive; a drive pulled and plugged back is opened again.

## Sound

- Sound through a USB sound adapter (USB Audio Class 1) or Bluetooth headphones. Without either the game
  runs silent at no cost.
- Volume from 0 to 20 (0 is off); the band says what plays the sound.

## Speed

- Automatic frame skip that draws every frame unless the game falls behind, and holds an even rate when
  it does; it can be turned off.
- Fast forward draws as many pictures as normal play, evenly, and runs the game as fast as it can.
- The CX II runs the game's hot code from on-chip memory; an older CX runs at 192 MHz by default. CPU
  speed and fast memory turn themselves down after a freeze and stay down.
- The FPS counter is Off, On or Detailed.

## Link

- Two calculators linked with a USB cable play the games' link modes: the GBA link cable as wired, and
  the wireless adapter's modes of Pokemon Ruby, Sapphire, Emerald, FireRed and LeafGreen. Advance Wars,
  Fire Emblem: The Sacred Stones and Golden Sun have their own link modes.
- Plugging the cable in links on its own; Settings > Link says the link's state.

## Building and testing

- The calculator build (`make`, with the Ndless SDK's tools on PATH) links five libraries that sit beside
  the gpsp folder, under these names: `nspire-usbpad-pad` (or `nspire-usbpad`), `nspire-usbms`,
  `nspire-usbshim`, `nspire-usbaudio-gpsp` (or `nspire-usbaudio`) and `nspire-bt`, with BTstack (tag
  v1.8.3) as `btstack` beside `nspire-bt`. README.md says more.
- `tests/gate.sh` runs every test in one command (the PC tests, every screen, the USB drive and sound
  tests, the ARM tests under qemu-arm, the calculator build's checks) and prints a summary table. It needs
  GBA_TESTS (github.com/jsmolka/gba-tests), QEMU, and the games in GBA_GAMES for the parts that use them;
  set TMPDIR to a folder on a disk with room.
