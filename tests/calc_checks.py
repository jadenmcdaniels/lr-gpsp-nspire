#!/usr/bin/env python3
"""Checks on the calculator build that the PC tests can't make, read from
its disassembly. Needs the Ndless toolchain on PATH (arm-none-eabi-objdump).

    calc_checks.py [gpsp_libretro.elf]

* gui.c's menu() calls its options' functions through trampolines: code GCC
  writes onto the stack, which Ndless maps write-back cached. The data cache
  must be written back and the instruction cache emptied before the first
  of those calls, or the CPU can run stale bytes (nspire_sync_written_code).
* video_fill_u16_strip_arm (nspire/scanline_fill16.S) stores 16 bytes at a
  time with stmia, which needs a word address: the calculator checks
  alignment and crashed when video.cc filled from an odd pixel (Pokemon
  battle wipes). An odd start must be stored alone before the first stmia.
  tests/arm/fill_test.c runs it; this checks the build has that version.
* The code every scanline event and interrupt runs (common.h NSPIRE_HOT,
  .text.hot.gpsp: update_gba, the block lookups, arm_stub.S...) has to be
  one stretch smaller than the 16 KB instruction cache, with nothing else
  (main once landed in the middle of it): the whole of it, update_gba up to
  arm_stub.S's nspire_hot_end (speed10: the check covered update_gba ..
  ext_store_iwram_u32 alone, 14 KB of a merge's 16.6 KB). And at every load
  base (the program's base modulo 4 KB changes from launch to launch) its
  pages must leave nspire_sram.c's SRAM both caches' first pages: at most
  SRAM_PAGES - 2 x SRAM_DYN_PAGES (5).
* The pinned layout: update_gba (first in that stretch), iwram (arm_stub.S's
  .bss) and rom_translation_cache (.jit) each start a 4 KB way, and the
  link's hooks stay out of update_gba (the pinned layout).
* The per-line I/O code (write_io_register16, dma_transfer,
  nspire_video_write_line_regs) at the places in a 4 KB way that
  nspire_lineio.ld gives, and crt0 still at the program's start (out-06).
* The speed model (tests/arm/build/perf, when linked after this ELF) has
  every function and variable both builds have at this build's place in a
  4 KB way
  (tests/arm/mirror_layout.py: the model runs the
  calculator's layout).
* The dynarec's fast push/pop (arm_emit.h nspire_block_fast) finds IWRAM at
  a fixed distance below reg (NSPIRE_IWRAM_DATA, arm_stub.S's .bss); if the
  layout moves, the fast path turns itself off, which would cost speed
  without anything else showing it.
* The link's NavNet service callback (nspire_link_usb.c service) runs in an
  OS task alongside the game's: it must make no syscall through Ndless's
  gateway (swi), which keeps one caller's return at a time, and call
  nothing directly but the OS's read and write through their pointers
  (blx). Its read and write addresses are asked of Ndless with the
  "address of" swi (0x800000 | 272, 273).
* The link asks the USB gamepad's driver whether a pad holds the port
  (nspire_usbpad_active, nspire_usbpad_owns_usb: weak defaults in
  nspire_link.c). GCC's LTO once inlined a weak default's "return 0"
  although the linker took a strong definition: some code must call them
  (or, built with pads, the library's usbpad_lib_active). Built with pads
  (nspire-usbpad beside gpSP), the driver and its shim are in the ELF.
"""

import os
import re
import subprocess
import sys

ELF = sys.argv[1] if len(sys.argv) > 1 else "gpsp_libretro.elf"


def function(dis, name):
    lines, on = [], False
    for line in dis.splitlines():
        if re.match(r"^[0-9a-f]+ <%s>:$" % re.escape(name), line):
            on = True
            continue
        if on:
            if not line.strip():
                break
            lines.append(line)
    return lines


def symbols():
    out = subprocess.run(["arm-none-eabi-nm", "-S", ELF], capture_output=True, text=True,
                         check=True).stdout
    syms = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 3:
            try:
                syms[p[-1]] = (int(p[0], 16), int(p[1], 16) if len(p) == 4 else 0)
            except ValueError:
                pass
    return syms


HOT = ["update_gba", "block_lookup_address_arm", "block_lookup_address_thumb", "set_cpu_mode",
       "check_and_raise_interrupts", "nspire_flush_lines", "arm_update_gba_check_thumb",
       "execute_spsr_restore", "mem_dispatch_load_u32", "ext_store_iwram_u32"]
IWRAM_DATA = 0xA0000   # arm_emit.h NSPIRE_IWRAM_DATA


def check_layout():
    syms = symbols()
    failed = 0
    missing = [n for n in HOT + ["reg", "iwram", "main"] if n not in syms]
    if missing:
        print("FAIL: no %s in %s" % (", ".join(missing), ELF))
        return 1
    lo = min(syms[n][0] for n in HOT)
    hi = max(syms[n][0] + max(syms[n][1], 4) for n in HOT)
    main_at = syms["main"][0]
    if hi - lo > 16 * 1024 or lo <= main_at < hi:
        print("FAIL: the hot code spans 0x%x-0x%x (%d bytes)%s" %
              (lo, hi, hi - lo, ", main is inside it" if lo <= main_at < hi else ""))
        failed += 1
    else:
        print("hot code: one stretch of %.1f KB (0x%x-0x%x)" % ((hi - lo) / 1024.0, lo, hi))
    # The pinned layout (the pinned layout): the hot stretch, the
    # GBA's memory and the dynarec's blocks each start a 4 KB cache way, as
    # in the speed model's build, so their cache sets don't move with
    # unrelated changes.
    for name in ("update_gba", "iwram", "rom_translation_cache"):
        if name not in syms:
            print("FAIL: no %s in %s" % (name, ELF))
            failed += 1
        elif syms[name][0] & 0xFFF:
            print("FAIL: %s at 0x%x, not at the start of a 4 KB way (pinned layout)" % (name, syms[name][0]))
            failed += 1
    if "update_gba" in syms and syms["update_gba"][0] != lo:
        print("FAIL: update_gba (0x%x) doesn't start the hot stretch (0x%x)" % (syms["update_gba"][0], lo))
        failed += 1
    if "nspire_hot_end" not in syms:
        print("FAIL: no nspire_hot_end in %s" % ELF)
        failed += 1
    else:
        # The whole stretch (update_gba .. nspire_hot_end: what nspire_sram.c
        # moves) in the instruction cache, and its pages at the worst load
        # base (any word in a page: nspire_sram.c's sram_pick counts from
        # update_gba's page to nspire_hot_end's) leaving both translation
        # caches their SRAM pages.
        end = syms["nspire_hot_end"][0]
        sram = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "nspire_sram.c")).read()
        total = int(re.search(r"#define SRAM_PAGES\s+(\d+)", sram).group(1))
        dyn = int(re.search(r"#define SRAM_DYN_PAGES\s+(\d+)", sram).group(1))
        budget = total - 2 * dyn
        worst = (0xFFC + end - lo + 0xFFF) // 0x1000
        if end < hi:
            print("FAIL: nspire_hot_end (0x%x) inside the hot code (0x%x-0x%x)" % (end, lo, hi))
            failed += 1
        if end - lo > 16 * 1024:
            print("FAIL: the hot stretch 0x%x-0x%x is %d bytes, more than the 16 KB instruction cache" %
                  (lo, end, end - lo))
            failed += 1
        if worst > budget:
            print("FAIL: the hot stretch 0x%x-0x%x (%d bytes) takes up to %d SRAM pages as the program "
                  "lands, more than %d (SRAM_PAGES %d less the caches' 2 x %d)" %
                  (lo, end, end - lo, worst, budget, total, dyn))
            failed += 1
        if end >= hi and end - lo <= 16 * 1024 and worst <= budget:
            print("hot stretch: update_gba .. nspire_hot_end %d bytes (0x%x-0x%x), at most %d SRAM pages "
                  "at any load base (budget %d)" % (end - lo, lo, end, worst, budget))
    gap = syms["reg"][0] - (syms["iwram"][0] + 0x8000)
    if gap != IWRAM_DATA:
        print("FAIL: IWRAM's data is 0x%x below reg, the fast push/pop expects 0x%x" % (gap, IWRAM_DATA))
        failed += 1
    else:
        print("IWRAM's data is 0x%x below reg, as the fast push/pop expects" % gap)
    return failed


def check_lineio():
    """The per-line I/O code at its pinned place in a 4 KB way (nspire_lineio.ld,
    common.h NSPIRE_LINEIO): each '. = ALIGN(...) + 0xOFF' line followed by
    '*(.text.lineio.NAME)' must put NAME at OFF modulo 4 KB, and the program's
    start (crt0) must stay at 0."""
    import os
    here = os.path.dirname(os.path.abspath(__file__))
    ld = os.path.join(here, "..", "nspire_lineio.ld")
    syms = symbols()
    want, off = [], None
    for line in open(ld):
        m = re.match(r"\s*\. = ALIGN\(.*\) - 0x1000 \+ (0x[0-9a-fA-F]+);", line)
        if m:
            off = int(m.group(1), 16)
            continue
        m = re.match(r"\s*\*\(\.text\.lineio\.(\w+)\)", line)
        if m:
            want.append((m.group(1), off))
            off = None
    failed = 0
    if len(want) != 3 or any(o is None for _, o in want):
        print("FAIL: %s doesn't place the three per-line I/O functions at an offset each" % ld)
        return 1
    for name, o in want:
        if name not in syms:
            print("FAIL: no %s in %s" % (name, ELF))
            failed += 1
        elif syms[name][0] % 0x1000 != o:
            print("FAIL: %s at 0x%x, not at 0x%03x in a 4 KB way (nspire_lineio.ld)" % (name, syms[name][0], o))
            failed += 1
    if syms.get("_start", (None,))[0] != 0 or syms.get("__crt0_savedsp", (0,))[0] >= 0x100:
        print("FAIL: crt0 doesn't start the program (nspire_lineio.ld must place it first)")
        failed += 1
    if not failed:
        print("per-line I/O code pinned: %s" % ", ".join("%s at 0x%03x" % w for w in want))
    return failed


def check_pins():
    """The game's functions pinned by nspire_lineio.ld (the game's code
    pinned): each '*(.text.NAME)' after a '. = ALIGN(...) + 0xOFF;' line
    at OFF in a 4 KB way; after a hot-stretch line ('. = ... >= . ? ...'), at OFF
    or up to 0x40 bytes after it (the stretch never jumps a way). A function
    renamed or gone (its pin then matches nothing) fails too."""
    import os
    here = os.path.dirname(os.path.abspath(__file__))
    ld = os.path.join(here, "..", "nspire_lineio.ld")
    syms = symbols()
    failed = exact = stretch = 0
    off = mode = None
    for line in open(ld):
        m = re.match(r"\s*\. = ALIGN\(.*\) - 0x1000 \+ (0x[0-9a-fA-F]+);", line)
        if m:
            off, mode = int(m.group(1), 16), "exact"
            continue
        m = re.match(r"\s*\. = \. - \(\. & 0xfff\) \+ (0x[0-9a-fA-F]+) >= \.", line)
        if m:
            off, mode = int(m.group(1), 16), "stretch"
            continue
        m = re.match(r"\s*\*\(\.text\.([A-Za-z_][\w.$]*)\)\s*$", line)
        if m and off is not None and not m.group(1).startswith("lineio."):
            name = m.group(1)
            if name not in syms:
                print("FAIL: %s is pinned in nspire_lineio.ld but not in %s (renamed? regenerate the pins)" % (name, ELF))
                failed += 1
            else:
                at = syms[name][0] % 0x1000
                if mode == "exact" and at != off or mode == "stretch" and not 0 <= at - off <= 0x40:
                    print("FAIL: %s at 0x%x, pinned at 0x%03x in a 4 KB way" % (name, syms[name][0], off))
                    failed += 1
                elif mode == "exact":
                    exact += 1
                else:
                    stretch += 1
        off = None if not line.strip().startswith(".") else off
    if not failed:
        print("game functions pinned: %d at their place in a 4 KB way, %d of the hot stretch at theirs "
              "or just after" % (exact, stretch))
    return failed


def check_mirror():
    """The speed model (tests/arm/build/perf) runs this build's layout
    (tests/arm/mirror_layout.py): every function both have at the same place
    in a 4 KB way. Checked when the model was linked after this ELF."""
    import os
    here = os.path.dirname(os.path.abspath(__file__))
    model = os.path.join(here, "arm", "build", "perf")
    if not os.path.exists(model) or os.path.getmtime(model) < os.path.getmtime(ELF):
        print("model layout: not checked (tests/arm/build/perf missing or older than %s;"
              " make -C tests/arm build/perf)" % ELF)
        return 0
    out = subprocess.run([sys.executable, "-I", os.path.join(here, "arm", "mirror_layout.py"), "check", ELF,
                          model, "quiet"], capture_output=True, text=True).stdout
    m = re.search(r"code both builds have: (\d+) bytes, ([0-9.]+)%", out)
    d = re.search(r"data both builds have: (\d+) bytes, ([0-9.]+)%", out)
    if not m or float(m.group(2)) < 99.5 or not d or float(d.group(2)) < 99.5:
        print("FAIL: the speed model doesn't run this build's layout (data: tests/arm/Makefile"
              " MIRROR_DATA=1):\n" + out)
        return 1
    print("model layout: %s%% of the code and %s%% of the data both have at this build's place in a 4 KB way"
          % (m.group(2), d.group(2)))
    return 0


def main():
    # Keep register names stable when GCC assigns r10/r11/r12: objdump's
    # default sl/fp/ip aliases otherwise hide valid instructions from the
    # checks below (the menu's cache invalidation used sl after USB integration).
    dis = subprocess.run(["arm-none-eabi-objdump", "-d", "-M", "reg-names-raw", ELF], capture_output=True, text=True,
                         check=True).stdout
    failed = check_layout()
    failed += check_lineio()
    failed += check_pins()
    # The link's hooks are called from update_gba, not inlined into it (serial.h
    # noipa): inlined, the hot stretch grew by 3.7 KB of link code and differed
    # from the speed model's.
    failed += check_mirror()
    upd = function(dis, "update_gba")
    calls = set(re.findall(r"\sbl\s+[0-9a-f]+ <([^>+]+)>", "\n".join(upd)))
    if not upd or not {"cable_update", "cable_next_event"} <= calls:
        print("FAIL: update_gba doesn't call cable_update and cable_next_event (inlined?)")
        failed += 1
    else:
        print("update_gba calls the link's hooks (not inlined)")
    menu = function(dis, "menu")
    if not menu:
        print("FAIL: no menu() in %s" % ELF)
        return 1
    first_indirect = next((i for i, l in enumerate(menu) if re.search(r"\sblx\s+r\d+", l)), None)
    clean = next((i for i, l in enumerate(menu) if "<clean_dcache>" in l or "cr7, cr10, {3}" in l), None)
    invalidate = next((i for i, l in enumerate(menu) if re.search(r"mcr\s+15, 0, r\d+, cr7, cr5, \{0\}", l)),
                      None)
    # gui.c is built without LTO (Makefile COLD_OBJS): menu() calls
    # nspire_sync_written_code (nspire/nspire.c) instead of having it inlined;
    # that call counts as both if the function itself cleans, then empties.
    sync = function(dis, "nspire_sync_written_code")
    s_clean = next((i for i, l in enumerate(sync) if "<clean_dcache>" in l or "cr7, cr10, {3}" in l), None)
    s_inv = next((i for i, l in enumerate(sync) if re.search(r"mcr\s+15, 0, r\d+, cr7, cr5, \{0\}", l)), None)
    call = next((i for i, l in enumerate(menu) if "<nspire_sync_written_code>" in l and re.search(r"\sbl\s", l)),
                None)
    if (clean is None or invalidate is None) and call is not None and s_clean is not None and s_inv is not None \
            and s_clean < s_inv:
        clean = invalidate = call
    if first_indirect is None:
        print("note: menu() makes no indirect calls")
    elif clean is None or invalidate is None or not clean <= invalidate < first_indirect:
        print("FAIL: menu() doesn't write back the data cache and empty the instruction cache before "
              "its first call through a trampoline (clean %s, invalidate %s, first call %s)"
              % (clean, invalidate, first_indirect))
        failed += 1
    else:
        print("menu(): caches synced before its first trampoline call")
    fill = function(dis, "video_fill_u16_strip_arm")
    stm = next((i for i, l in enumerate(fill) if re.search(r"\sstm(ia)?\s+r0!", l)), None)
    align = next((i for i, l in enumerate(fill) if re.search(r"\stst\s+r0, #2\b", l)), None)
    if not fill:
        print("FAIL: no video_fill_u16_strip_arm in %s" % ELF)
        failed += 1
    elif stm is None:
        print("note: video_fill_u16_strip_arm has no stmia")
    elif align is None or align > stm:
        print("FAIL: video_fill_u16_strip_arm stores with stmia without aligning an odd start first")
        failed += 1
    else:
        print("video_fill_u16_strip_arm: an odd start is stored alone before the first stmia")
    failed += check_link_service(dis)
    failed += check_usbpad_hooks(dis)
    failed += check_drive_attach(dis)
    print("%d failed" % failed)
    return 1 if failed else 0


def check_link_service(dis):
    names = [m.group(1) for m in re.finditer(r"^[0-9a-f]+ <(service(?:\.[a-z_]+\.\d+)?)>:$", dis, re.M)]
    if len(names) != 1:
        print("FAIL: %d NavNet service callbacks (service*) in %s" % (len(names), ELF))
        return 1
    body = function(dis, names[0])
    swis = [l for l in body if re.search(r"\s(svc|swi)\s", l)]
    calls = [l for l in body if re.search(r"\sbl\s", l)]
    indirect = [l for l in body if re.search(r"\sblx\s+r\d+", l)]
    failed = 0
    if swis or calls:
        print("FAIL: the NavNet service callback makes %d syscalls and %d direct calls:\n  %s"
              % (len(swis), len(calls), "\n  ".join(swis + calls)))
        failed += 1
    elif not indirect:
        print("FAIL: the NavNet service callback never calls the OS's read")
        failed += 1
    else:
        print("link service callback: no gateway syscalls, %d calls through the OS's read/write" % len(indirect))
    for number in (0x110, 0x111):
        if not re.search(r"\s(svc|swi)\s+0x%08x\b" % (0x800000 | number), dis):
            print("FAIL: no \"address of\" swi for syscall %d" % number)
            failed += 1
    return failed


def check_usbpad_hooks(dis):
    syms = symbols()
    failed = 0
    with_pads = "usbpad_poll" in syms
    if with_pads:
        missing = [n for n in ("usbpad_lib_active", "usbshim_register", "usbpad_init") if n not in syms]
        if missing:
            print("FAIL: built with USB gamepads but no %s" % ", ".join(missing))
            failed += 1
        else:
            print("USB gamepads: the driver (usbpad_lib_active) and its shim (usbshim_register) are linked")
    else:
        print("note: built without USB gamepads (no ../nspire-usbpad)")
    active = ["usbpad_lib_active", "nspire_usbpad_active"] if with_pads else ["nspire_usbpad_active"]
    for names in (active, ["nspire_usbpad_owns_usb"]):
        pattern = r"\sbl\s+[0-9a-f]+ <(%s)>" % "|".join(re.escape(n) for n in names)
        if not re.search(pattern, dis):
            print("FAIL: nothing calls %s (LTO inlined a weak default?)" % " or ".join(names))
            failed += 1
        else:
            print("the link calls %s" % " or ".join(names))
    return failed


def check_drive_attach(dis):
    """R11u (2026-11-15): with the USB drive driver built in, its attach is
    nspire_drive_os.c's wrapper (it never returns an error after the match
    said yes), registered in its place. It runs in the OS's USB task: no
    syscall through Ndless's gateway."""
    if "nspire_drive_kept" not in symbols():
        print("note: built without the USB drive driver (no nspire_drive_kept)")
        return 0
    names = [m.group(1) for m in re.finditer(r"^[0-9a-f]+ <(gpsp_ms_attach(?:\.[a-z_]+\.\d+)?)>:$", dis, re.M)]
    if len(names) != 1:
        print("FAIL: %d drive attach wrappers (gpsp_ms_attach*) in %s" % (len(names), ELF))
        return 1
    body = function(dis, names[0])
    swis = [l for l in body if re.search(r"\s(svc|swi)\s", l)]
    if swis:
        print("FAIL: the drive attach wrapper makes %d syscalls:\n  %s" % (len(swis), "\n  ".join(swis)))
        return 1
    print("drive attach wrapper (%s): linked, no gateway syscalls" % names[0])
    return 0


if __name__ == "__main__":
    sys.exit(main())
