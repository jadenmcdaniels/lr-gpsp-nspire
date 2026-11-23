# lr-gpsp-nspire

To use, transfer the latest 2.0 release on a calculator with ndless installed and click enter on the file to run it.

Standalone [libretro gpsp](https://github.com/libretro/gpsp) for TI-Nspire calculators (CX, CX CAS, CX II, etc.).

**Version 2.0 beta.** It is a beta: tested on the PC and on a CX CAS and a CX II, not in every combination of
devices; Bluetooth with a USB hub is the least proven part. [CHANGES.md](CHANGES.md) lists what it does
(and where a Realtek Bluetooth dongle's firmware files go); `docs/ui/r19.txt` describes the menus as they
are now (some sections below describe earlier builds' menus, which r19 replaced).

The GUI and host code come from legacy gpSP; the emulation core is libretro-gpsp, without a libretro frontend.



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
