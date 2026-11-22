# lr-gpsp-nspire

Standalone [libretro gpsp](https://github.com/libretro/gpsp) for TI-Nspire calculators (CX, CX CAS, CX II, etc.).

**Version 2.0 beta.** It is a beta: tested on the PC and on a CX CAS and a CX II, not in every combination of
devices; Bluetooth with a USB hub is the least proven part. [CHANGES.md](CHANGES.md) lists what it does
(and where a Realtek Bluetooth dongle's firmware files go); `docs/ui/r19.txt` describes the menus as they
are now (some sections below describe earlier builds' menus, which r19 replaced).

The GUI and host code come from legacy gpSP; the emulation core is libretro-gpsp, without a libretro frontend.

## Build

Requires the [Ndless SDK](https://ndless.me/) (`nspire-gcc`, `nspire-g++`, `genzehn`).

**Recommended (Docker):**

```bash
docker compose run --rm ndless-dev make
```

Tag your Ndless SDK image as `ndless-dev:latest` (e.g. from [stepney141/ndless-docker](https://github.com/stepney141/ndless-docker)), or set `NSPIRE_BUILD_IMAGE`.

**The libraries.** The calculator build links five libraries and BTstack, each a
directory beside this one:

    gpsp/                   this repository
    nspire-usbpad-pad/      USB controllers (../nspire-usbpad is taken when this one is missing)
    nspire-usbms/           USB drives (with FatFs)
    nspire-usbshim/         the OS's USB host calls, shared by the drivers
    nspire-usbaudio-gpsp/   USB sound adapters (../nspire-usbaudio is taken when this one is missing)
    nspire-bt/              Bluetooth controllers and headphones
    btstack/                BTstack (github.com/bluekitchen/btstack, tag v1.8.3), beside nspire-bt

To get them, from the directory that holds `gpsp/`:

    git clone https://github.com/jadenmcdaniels/nspire-usbpad
    git clone https://github.com/jadenmcdaniels/nspire-usbms
    git clone https://github.com/jadenmcdaniels/nspire-usbshim
    git clone https://github.com/jadenmcdaniels/nspire-usbaudio
    git clone https://github.com/jadenmcdaniels/nspire-bt
    git clone https://github.com/bluekitchen/btstack && git -C btstack checkout v1.8.3

A library that is missing is left out of the build, with its feature; `USBPAD=0`, `USBMS=0`,
`USB_AUDIO=0` and `BT=0` leave one out on purpose, and `USBPAD_DIR`, `USBMS_DIR`, `USBSHIM`,
`USB_AUDIO_DIR` and `BT_DIR` name another place. The Bluetooth library is built by its own
Makefile (`make -C ../nspire-bt lib`, called from gpSP's) and finds BTstack at `../btstack` from
its own directory.

**Host build:**

```bash
make
```

Output: `gpsp_libretro.tns`. Copy it to the calculator as `_gpSP.tns`, in the
folder with your games: the underscore puts it at the top of the folder, so
opening the folder and pressing enter starts gpSP.

See [README.txt](README.txt) and [DOCKER_BUILD.md](DOCKER_BUILD.md) for device file layout, RAM flags, and Windows/Docker notes.

## Device files

    My Documents/
      gba/
        _gpSP.tns                    the program, first in the folder; the settings
                                     for all games are kept inside it
        Game.gba.tns                 ROMs, or zipped (Game.zip.tns), or .gbz.tns
      zgpSP_files/                   made on the first start, wherever gpSP is (the z
                                     lists it below your game folders)
        gba_bios.bin.tns             the GBA BIOS (optional; one anywhere in My Documents
                                     or a folder in it moves here)
        Game.gba.sav.tns / .sav.gz.tns   in-game saves, plain or gzipped
        Game.gba0.svs.tns ...        save states, slots 0 to 999
        Game.gba.settings.tns        a game's own settings and keys
        gpsp.cfg.tns                 a copy of the settings for all games
        gpsp_order_gba.txt.tns       the gba folder's game order
        gpsp_romdir.tns, gpsp_results.txt.tns, gpsp_perf.txt.tns, gpsp_clock.tns

The game folder has only the program and the games. gpSP doesn't need a
`gba` folder: it can be in any folder, or right in My Documents, and the
saves folder is always My Documents/zgpSP_files. Builds before kept these
in `zgpSP_files` or `gpSP_saves` (moved in whole at start, then the old
folder goes; `gpsp_qol.cfg.tns` becomes `gpsp.cfg.tns`), or next to the ROMs
and next to gpSP: the first start moves them into `zgpSP_files` (a game in
another folder, when it starts),
with the BIOS and older gpSP builds' settings files (`gpsp.cfg.tns`,
`Game.gba.cfg.tns`, `Game.cfg.tns`, and `..cfg.tns` from My Documents too).
Other programs' files stay.

## What this build adds (TI-Nspire QoL, 2026-10)

The menus are gpSP's own; the new settings are extra lines in them.

**No more crashes at scene changes.** Since v0.4 games could reset or freeze
the calculator at Pokemon battle wipes, door transitions and some cutscenes
(issues [#3](https://github.com/andymcca/lr-gpsp-nspire/issues/3),
[#4](https://github.com/andymcca/lr-gpsp-nspire/issues/4),
[#5](https://github.com/andymcca/lr-gpsp-nspire/issues/5)). There were two
causes, both fixed: the *libretro video.cc* renderer's fast fill crashed when
a transition's window edge fell on an odd pixel, and *Partial SMC* (Emulator
options, *Dynarec RAM / SMC*) ran code that games keep in RAM wrongly, so
Pokemon Emerald froze right after loading a state. Both
*Dynarec RAM / SMC* settings and every ROM buffer size work now.

**A damaged ROM stops with a message.** A game that jumps where no code can
run (a bad dump of Advance Wars does as it starts) used to reset the
calculator. Now gpSP shows "Stopped", the game and where it jumped, and the
file list comes back; no state is saved of it. A good copy of the game is
the fix for that one.

**Screen.** On the CX II the picture is turned for the portrait LCD in software
and shown from one of three buffers, which the LCD switches between only
between refreshes: no diagonal tearing (that came from Ndless's compatibility
mode, which turned the picture inside the panel), and no tearing at all.
Frames are paced with a 32768 Hz timer at the GBA's 59.73 frames a second
instead of waiting for the LCD after every frame, so **automatic frameskip
works** now (it never skipped before): it draws every frame unless the game
falls behind.

*Display scaling* now does what it says. Older builds showed the sharp
320x214 picture (*scaled/unblended*) whatever it was set to, so settings from
them, and from the first QoL build, start with that one; *unscaled* is the
GBA's own 240x160 in the middle.

**Faster blended pictures.** *scaled/blended 3:2* and *fullscreen/blended*
(and *Interframe blending* with them) are drawn in one pass now
(`nspire_blend.c`): the GBA frame is read once, blended, and written straight
into the turned screen buffer. Before, each frame went through four passes
over memory (frame mixing, a copy into the old 5:5:5 format, notaz's scaler,
turning and converting back), about 10 ms of a 16.7 ms frame on the
calculator; the CPU waits about 120 cycles for every 32 bytes it reads from
memory, and its data cache is only 8 KB. The math is notaz's, in the LCD's
5:6:5: red and blue are exactly as before, green keeps its sixth bit. The 3:2
picture is one line higher (lines 12-224), so each column's pixels are whole
words. The FPS counter's and messages' text bands are only drawn again when
their text changes.

**The menus (the "plain" design; `docs/ui/r19.txt` is how they are now).** Black and white,
one monospaced face (IBM Plex Mono) at one size, capitals only. The picked thing is a black
block with white type: one on a screen, so it is always clear where the arrows are.

There is one screen. Its top row has a word in each corner, *Settings* and *Quit*, and between
them *Games* (with a game paused, *Saves* before it). Left and right move along the row; what
is under a word shows at once. **down** or **enter** goes into it, **up** from its top row or
**esc** comes back to the row. **esc** on the row goes back to the game.
- *Games* is a grid, three by two, of each game's newest save's picture with its name under it;
  only the picked one is in colour. The folders come after the games, as items too. **enter**
  plays the game (**shift+enter**: without resuming its last save) or opens the folder, **S**
  opens the game's saves, **V** shows the games a line each, **tab** picks a game up to move it
  (**shift+tab** sorts A to Z), **U** goes to the USB drive.
- *Saves* is what the **menu** key opens in a game: the same grid with *Resume*, *Save* (a
  new slot), the game's own save and every state. **enter** resumes, saves or loads; **S** on
  a state saves over it, **D** deletes it, **M** marks it (**A**: all), **C**/**U** pack and
  unpack, **T** copies it to the other place and **tab** switches between the calculator and a
  USB drive. **esc** goes back to the game.
- *Settings* is one screen of tiles, a name over its value: *Volume*, *Brightness*, *Screen
  size* (original, fit, stretch), *FPS counter*, *Autosave*, then *Keys*, *Controller*, *Bluetooth*
  (builds with it), *Link* (with a game), *Cheats* (when the game has codes) and *More*.
  **enter** changes the picked setting: one with a few values goes to the next one at each
  press; a range (volume, brightness, autosave) goes between `< >` and left/right change it.
  *More* is one page ("SETTINGS / MORE"): where the save key and the autosave put a save, the
  link's device, a game's own settings and keys, frame skip (auto or off), restart, the saves'
  and the games' managers and About.
- *Cheats* shows when the game has a cheat file: RetroArch's `.cht` (the ones in
  libretro-database), named as the game ("Game.cht", or "Game (Code Breaker).cht"), in the
  game's folder, in `zgpSP_files`, or on a USB drive (its top folder or `cheats`). Each code
  is a line with a switch; **enter** turns it on or off, and the file remembers. CodeBreaker
  codes only; up to 20 on at once.
- *Quit* asks nothing (it saves first when *Save on quit* is on).

New states keep a small picture of the game (after the state, so older builds still load them);
the loading screen shows the one a game resumes from. Over the game, only in the bands beside
the picture and only redrawn when their text changes: the speed (top left), words for a link
and a controller (top right) and messages (bottom).

**Brightness (CX II).** **+** and **-** make the screen brighter or dimmer, in
games ("Brightness 7/10" shows at the bottom), menus and the file list.
*Brightness* (Settings) sets it too: *calculator's* (the
default) leaves the calculator's own brightness alone, 1 to 10 are gpSP's.
When gpSP closes, the calculator's own brightness comes back. (It is the LCD
backlight at 0x90130014, which the OS sets from 0, brightest, to 225,
darkest.)

**File list.** Games are listed A to Z. **tab** picks a game up to move it: the arrows
move it, **enter** puts it there, **esc** puts it back. The folder keeps that order (in
`zgpSP_files`); **shift+tab** sorts it A to Z again. A folder opens on its first game, and
gpSP opens in the folder it was last in.

**Save states.** Slots 0 to 999, named as before (`Game.gba12.svs.tns`, in
`zgpSP_files`), so older states still load. In Settings, More:

| Setting | Default | |
|---|---|---|
| Use a new slot each time | on | every save goes to a new slot after the highest, so nothing is overwritten |
| Save when I quit | on | Quit and Choose another game save first |
| Resume last save | on | starting a game resumes its newest state; **shift+enter** in the file list starts without it |

In the main menu *Save state* shows the slot a save goes to (a new one with
auto-increment) and *Load state* the slot picked, with its picture beside
it; hold left/right to move faster.

The newest state is the one this build saved last: its states count their
saves, which stays right when a calculator reset restarts its clock. Until a
game has one of this build's states (it has only older builds' or the first
QoL build's), the newest is the most recently written file.

**Smaller files.** With *Pack saves* on (Settings, More; the
default, as in PocketSNES), states are zlib-compressed (gpSP's
states were 416 KB each) and in-game saves gzipped: `Game.gba.sav.gz.tns`,
which a PC unpacks like any `.gz` (`gunzip`, 7-Zip). Off, new states are
stored as they are and in-game saves are plain `.sav` files, which other
gpSP builds and PC emulators read. Writing a save removes its other form;
loading takes the newer of the two. Older states still load.

**Save manager** (main menu): the game's in-game save and states, with their
sizes, compressed or not, and the picked state's picture. **enter** marks files (**A**: all of them), **C**
compresses and **U** uncompresses the marked ones (or the one picked), and
**del** deletes them after asking (**del** again). A state keeps its place
among the newest when it's compressed or uncompressed. A deleted in-game
save comes back if the game saves again while it's open.

**ROM manager** (main menu): the games in the folder (the running game's,
or the file list's), their sizes, compressed or not. **enter** marks games
(**A**: all of them), **C** compresses the marked ones (or the one picked)
into `.gbz.tns` files, the same files `tools/gbz.py` makes (a `.gba` or a
zip: `Game.zip.tns` with `Game.gba` in it becomes `Game.gbz.tns`), and **U**
uncompresses them into `.gba.tns` files (a zip too). A `.gbz` is what gpSP
reads with the least memory: 32 KB at a time, where a zip is unpacked whole.
The new file is read back and checked before the old one is deleted, the
game order follows, and saves keep working (they go by the ROM's name). The game that's running can
be done too while its whole ROM is in memory (the ROM buffer setting).

**Zipped games.** A zip from the internet renamed to `Game.zip.tns` plays
like the ROM in it (the first `.gba` or `.agb` file; a zip without one is refused); its
saves, states and settings are the ROM's (`Pokemon.gba` in the zip uses
`Pokemon.gba.sav.tns` and so on), so a game can be zipped without losing
them. A zip is unpacked into memory whole when the game starts, and its
CRC is checked; when the ROM buffer is smaller than the ROM, more memory is
taken for it if there is any. When there isn't (free memory depends on the
documents open in the OS), gpSP makes a `.gbz` of the zip, once, on the
loading screen (Q cancels), named after the ROM in it as `tools/gbz.py`
names it (`Pokemon.gba` in the zip: `Pokemon.gbz.tns`, with the same saves
and states), checks it, deletes the zip and plays the `.gbz`. Without the
storage for it, or with a ROM in the zip that isn't a `.gba`, or its `.gbz`
there already, nothing changes and the loading screen says why and what to
do. Opening a `.zip.tns` from the calculator's
documents isn't set up, as PocketSNES may have that file type.

**Loading screen.** Starting a game shows its name (and the picture of the
state it resumes from) and a bar while its ROM is read into memory (with a big ROM buffer, all of it: 16 MB takes a
while), then the state it resumes from. A game that can't be loaded says
why (no ROM in the zip, not enough memory...), and the file list comes back.

ROMs can also be compressed on a PC, into `.gbz` files that are read in
pieces like a big `.gba` (no need to unpack them whole):

    python3 tools/gbz.py pack "Pokemon - Emerald.gba"     # -> Pokemon - Emerald.gbz.tns
    python3 tools/gbz.py pack -o out/ *.gba               # many at once
    python3 tools/gbz.py unpack Game.gbz.tns              # back to Game.gba

A `.gbz.tns` is read 32 KB at a time like a big `.gba` (`nspire_gbz.h` has the
format) and shares the `.gba`'s saves, states and settings. Older gpSP builds
can't read compressed states or ROMs.

**Keys.** *Keys* (in Settings) has two keys per action (enter picks the
line, then the key to press), key combinations (hold a key and press another),
and new hotkeys: brightness up and down (+ and -), next and previous save
slot (**)** and **(**, as in PocketSNES; settings from before them get these
keys where they're free), and, unbound at first, load newest state, FPS
counter, restart game. Save, load and slot changes show a message on screen. The page shows the calculator's
keypad with every binding where it is. 8 5 4 6 are up, down, left and right; 7 9 1 3 do
nothing unless you bind them. esc goes back in menus, and held arrows repeat.

**Per game.** *Settings are for* and *Keys are for* (Settings, More)
can be "this game only": the game keeps its own graphics, emulator and save
options or keys in `Game.gba.settings.tns` in `zgpSP_files`.

**Settings.** The settings for all games are kept inside `_gpSP.tns` itself
(an area after the program, which Ndless doesn't load), so they go wherever
the program goes, with a copy in `zgpSP_files/gpsp.cfg.tns`: a new
`_gpSP.tns` copied over the old one takes them from the copy. The first
start of this build takes them from `gpsp_qol.cfg.tns` or `gpsp.cfg.tns`
next to gpSP (builds before; older builds' `gpsp.cfg.tns` has an older
format, told apart by its size and word 37), and puts them into the program
file right away.

**Speed.** *Show speed on screen* (Settings): off, on ("60" at full speed, "28/30": frames drawn
a second / the most there can be, when below it; in fast forward, game frames a second /
60 as "2.0x") or detailed (ms per frame running the game,
drawing, showing, idle; logged to `gpsp_perf.txt.tns`). *Fast forward max*
(Settings, More): 1.1x to 10.0x in steps of 0.1x, or max
(the default: as fast as the game runs). Fast forward never skips game
frames and draws as many pictures as normal play, evenly: with frameskip
off a new picture at every LCD refresh (60 a second), with frameskip on
what normal play's frameskip draws in that scene (an older CX: one in two,
about 25-30 a second; automatic frameskip keeps following the scene as in
normal play). The game runs as fast as the rest of the time allows, and in
the PC model's scenes never slower than normal play in the same scene; the
speed shown is the real one. A top speed is
only a cap: a game that can't reach it runs as fast as it can, still
drawing every picture.
*CPU speed* (Settings, More): auto (the default), normal, or 408 to 504
MHz in 12 MHz steps on a CX II (168 to 264 MHz on an older CX) while a game
runs (PocketSNES's clock switch and list). Auto is normal on a CX II and 192
MHz (bus 64 MHz) on an older CX, whose 132 MHz can't hold 30 pictures a second
in Emerald's towns (the PC model says 192 MHz draws all 30 there; real units
have run 25-40% slower than that model, so expect about 23-26 a second until
a calculator has measured it; linking at the 64 MHz bus is untested). A speed
chosen before this build is kept, and a file that said normal (the old
default) now means auto. Pick normal to keep 132. On a
CX II try 432 first:
on the calculator this was made on, 456 glitched now and then and 480 froze
in PocketSNES. If a speed freezes the calculator, the next start goes back to
normal (marker file `gpsp_clock.tns`).

**Other.** Opening a `.gba.tns` or `.gbz.tns` from the calculator's documents
starts gpSP. Ndless finds the program for a file type by its name, so gpSP
registers the name it was started as; file types that name an earlier gpSP
(`gpsp_qol`, or any name starting with `gpsp_` or `_gpsp`) are moved to it,
so renaming it keeps games opening, and another program's are left alone.

**Fixed since the first QoL build** (the one that glitched): the menu could run
stale code on the calculator. gpSP's menu calls its options' functions through
small pieces of code GCC writes on the stack (trampolines, for nested
functions), and Ndless maps the stack write-back cached; the menu now writes
the data cache back and empties the instruction cache before using them
(`nspire_sync_written_code`). Older builds got away with it by always opening
the menu at the same stack address. The menu also no longer clears the
screen buffer being shown, which made it flicker. Help texts fit the screen,
and the second keys in the key settings line up.

### PC tests

`tests/gate.sh` runs all of them, side by side, and ends with one summary table (`--part host` and the
like run one part; `--help` lists them). It needs the Ndless toolchain on PATH (or `NSPIRE_ENV`: a script
that puts it there), `GBA_TESTS` (a copy of jsmolka's gba-tests), `QEMU` (qemu-arm), and the games in
`GBA_GAMES` for the parts that play them; set `TMPDIR` to a folder on a disk with room.

`tests/host/run.sh` builds and runs three sets of tests on a PC (needs gcc,
g++, zlib, python3 and [jsmolka's gba-tests](https://github.com/jsmolka/gba-tests)
in `$GBA_TESTS`):

* `core_tests`: the core with its interpreter: compressed and zipped ROMs
  (stored, deflated, damaged, without a ROM, bigger than the ROM buffer),
  save states packed and stored, in-game saves plain and gzipped (which one
  loads, damaged ones), and what the save manager does to both.
* `frontend_tests`: frame turning, key bindings, the settings files and their
  older versions (the slot keys added where free), brightness, the file
  list's order.
* `host_tests.py`: whole scenarios on a simulated CX II (`gpsp-host`,
  `host_hw.c`): gpSP's real main loop, menus and file list against a
  simulated LCD (which notices writes to the buffer being shown, i.e.
  tearing), 32 kHz timer, keypad, touchpad and backlight, with a script of
  key presses in virtual time and the calculator's costs for emulating,
  drawing and turning frames. They check screenshots and a report (which
  menu screen is shown and its picked line): the picture's size, the menu
  highlight moving with short presses, the settings pages with tab,
  brightness,
  states, number keys, moving and sorting games, folders, `.gbz` ROMs, auto
  frameskip, the FPS counter and fast forward, the speed test, the blended
  pictures with their text bands, the loading screen, zipped games and one
  that won't load, a zip too big for memory becoming a `.gbz` (`gpsp-host
  --rom-memory MB --free-storage KB`: a calculator short of memory or
  storage), another game started from the paused menu with the folder in
  the calculator OS's own form (`gpsp-host --os-cwd 1`: "\documents\gba\"),
  a damaged ROM that stops, the slot keys, the save
  manager, and that every menu line's text fits (the PC build says so when
  a text is cut short). `python3 tests/host/host_tests.py menu` runs one.
* `build/ui_preview OUTDIR GAME.ppm BATTLE.ppm` draws the menus' screens with
  real games' pictures (240x160 P6 frames) exactly as the calculator draws
  them, to look at the design.

`tests/calc_checks.py` (needs the Ndless toolchain) checks the calculator
build's disassembly for what a PC can't show: that `menu()` syncs the caches
before its first trampoline call, that video.cc's fill stores an odd
first pixel alone before its 16-byte stores, that the code every interrupt
runs is one stretch smaller than the instruction cache, and that IWRAM is
where the dynarec's fast push/pop expects it.

`tests/arm/run.sh` (needs the Ndless toolchain and qemu-arm) builds code for
the calculator's CPU and runs it under qemu-arm: the one-pass blended
pictures against the old path (notaz's ARM scalers, the 5:5:5 copy and
turning) on noise, gradients and extreme colors, with and without
interframe blending: same red and blue, green within 3 of 63, every pixel as
a plain per-pixel version of the math says, and nothing written outside the
picture. Also video.cc's backdrop fill at every start pixel and length (it
crashed on odd ones), and the core with its dynarec on a small test ROM
whose code in RAM calls, loops and is rewritten by DMA and by the CPU, with
both *Dynarec RAM / SMC* settings, and on one that jumps where no code can
run (it has to stop there). With the game, its BIOS and a save state
in `$GBA_GAMES` (see `tests/arm/run.sh`), Pokemon Emerald is played from the
state into a wild battle with both SMC settings, 8 and 32 MB ROM buffers and
as a zip: all must reach the same battle, frame for frame the hashes stored in
`tests/arm/replay/emerald_grass.frames`.
A damaged Advance Wars there has to stop at its bad jump. Eight scenes
(Advance Wars from a saved state and from power-on, Emerald, Fire
Emblem, Golden Sun, Minish Cap) are played with the picture's lines drawn
in batches and line by line: every frame must be the same.

`tests/speed_scenes.sh` estimates from the PC how fast those scenes run on
the calculator (milliseconds a frame for the game, drawing and the
picture), with `tests/calc_speed.sh`'s model of the ARM926.

## Licences

- gpSP and this port: the GNU General Public License, version 2 (`libretro-gpsp/COPYING`).
- Bluetooth: a build with Bluetooth (the default when `../nspire-bt` and `../btstack` are there, and the
  released program) contains BTstack, copyright BlueKitchen GmbH, under BTstack's own licence, reproduced
  in `docs/licences/BTSTACK-LICENSE.txt`. That licence allows personal, non-commercial use only; a
  build with `BT=0` contains none of it.
- USB drives: FatFs, copyright ChaN, under its own licence (`fatfs/LICENSE.txt` in nspire-usbms).
- The five libraries (nspire-usbpad, nspire-usbms, nspire-usbshim, nspire-usbaudio, nspire-bt) are released
  into the public domain (the Unlicense), apart from the third-party parts each one's README names.
- The menus' type is drawn from IBM Plex Mono, under the SIL Open Font License 1.1 (`docs/fonts/`).

## Releases

Older releases (including v0.5) remain on the [Releases](https://github.com/andymcca/lr-gpsp-nspire/releases) page.
