# Self-contained TI-Nspire build: all sources live under this directory.
# Typical build: see README.txt (docker compose run --rm ndless-dev make).
# With Ndless SDK on PATH: make

CORE_DIR := libretro-gpsp
# Vendored tree may ship a stub libretro-common; use sibling repo copy when needed.
COMM_DIR_LOCAL  := $(CORE_DIR)/libretro/libretro-common
COMM_DIR_PARENT := ../libretro-gpsp/libretro/libretro-common
ifeq ($(wildcard $(COMM_DIR_LOCAL)/include/streams/file_stream.h),)
  COMM_DIR := $(COMM_DIR_PARENT)
else
  COMM_DIR := $(COMM_DIR_LOCAL)
endif

# Vendored core may omit arm/ (dynarec); use sibling ../libretro-gpsp like COMM_DIR.
ARM_EMIT_LOCAL := $(CORE_DIR)/arm/arm_emit.h
ARM_DIR_PARENT := ../libretro-gpsp/arm
ifeq ($(wildcard $(ARM_EMIT_LOCAL)),)
  INCLUDES_ARM := -I../libretro-gpsp
  ARM_VPATH := $(ARM_DIR_PARENT)
else
  INCLUDES_ARM :=
  ARM_VPATH := $(CORE_DIR)/arm
endif

GCC  := nspire-gcc
GXX  := nspire-g++
GENZ := genzehn
AR   := arm-none-eabi-ar
OBJCOPY := arm-none-eabi-objcopy

BASE_FLAGS := -DNSPIRE_BUILD -DNSPIRE_LIBRETRO -DNSPIRE_NO_AUDIO -DARM_ARCH \
              -DHAVE_DYNAREC=1 -DSMALL_TRANSLATION_CACHE -DROM_BUFFER_SIZE=2 \
              -DNSPIRE_NO_INTERPRETER -DNSPIRE_PINNED_LAYOUT
INCLUDES   := -I$(CORE_DIR) -I$(CORE_DIR)/libretro -I$(COMM_DIR)/include \
              $(INCLUDES_ARM) -I. -Inspire

# TI-Nspire CX (etc.): ARM926EJ-S, ARMv5TE (+ Thumb, integer DSP). No VFP/NEON.
# -fno-reorder-functions: no .text.startup / .text.unlikely sections; Ndless's
# linker script puts every .text.* section together, and main in .text.startup
# landed in the middle of .text.hot.gpsp (common.h NSPIRE_HOT).
ARM926FLAGS := -march=armv5te -mtune=arm926ej-s -fomit-frame-pointer -ffast-math -fno-reorder-functions

CFLAGS := -O3 -flto -std=c99 -msoft-float -funsigned-char -fno-common \
          $(ARM926FLAGS) \
          -D_GNU_SOURCE \
          $(BASE_FLAGS) $(INCLUDES) -Wall -Wfatal-errors -MMD -MP

CXXFLAGS := -O3 -flto -std=gnu++11 -msoft-float -funsigned-char -fno-common \
            $(ARM926FLAGS) \
            $(BASE_FLAGS) $(INCLUDES) -fno-exceptions -fno-rtti -Wall -Wfatal-errors -MMD -MP

GUI_CPPFLAGS := -DCOMPILING_GUI_MODULE

# -ffunction-sections at the link: every function of LTO's its own section,
# which nspire_lineio.ld places by name (the code is the same).
LDFLAGS := -O3 -flto $(ARM926FLAGS) -ffunction-sections
# R6b, r16: newlib's printf family, its integer-only stream version for all
# of it (nspire_cold.mk PRINTF_LDFLAGS, added after its include below).
# R6b: library code nothing calls (unused newlib, libgcc, Ndless syscall stubs and
# nspire-usbpad functions; Ndless's own samples link this way). -5,015 bytes of the .tns.
LDFLAGS += -Wl,--gc-sections
# fast17: gpSP's removes and renames tell the OS file list's check
# (nspire_fswrap.c, nspire_paths.h nspire_file_gone)
LDFLAGS += -Wl,--wrap=remove,--wrap=rename,--wrap=rmdir
# out-06, size7: the game's code and data at fixed places in the caches'
# ways (nspire_lineio.ld, common.h NSPIRE_LINEIO and NSPIRE_PINNED_LAYOUT;
# ld 2.43 or later). The map: tools/pin_layout.py, tests/layout_pinned.py.
LINEIO_LD := nspire_lineio.ld
LDFLAGS += -Wl,--section-ordering-file=$(LINEIO_LD) -Wl,-Map=gpsp_libretro.map
LDLIBS  := -lz -lm

ZEHNFLAGS := --version 12 --author "gpSP libretro port" --clickpad-support=false \
             --color-support=true --240x320-support=true --ndless-min 31 --ndless-rev-min 2001
# The program file, packed: genzehn's
# uncompressed file packed by tools/zehn_pack.py: no .bss zeros, zopfli
# (pigz -11, else zlib 9), the relocations in the compressed image for
# nspire/zehn_entry.S. ZEHN_PACK=0 (or no python3): genzehn --compress.
ZEHN_PACK ?= $(if $(shell command -v python3),1,0)

VPATH := $(CORE_DIR):$(ARM_VPATH):$(COMM_DIR)/compat:$(COMM_DIR)/encodings:\
$(COMM_DIR)/file:$(COMM_DIR)/streams:$(COMM_DIR)/string:$(COMM_DIR)/time:\
$(COMM_DIR)/vfs

COMM_OBJS := \
  compat_posix_string.o compat_strl.o fopen_utf8.o encoding_utf.o \
  file_path.o file_path_io.o file_stream.o stdstring.o rtime.o vfs_implementation.o

CORE_OBJS := \
  main.o gba_memory.o savestate.o sound.o cheats.o memmap.o serial.o \
  gbp.o rfu.o serial_proto.o serial_fe.o serial_msio.o gba_cc_lut.o cpu_threaded.o

CORE_ASM_OBJS := bios_data.o arm_stub.o

CORE_CXX_OBJS := cpu.o

# USB gamepads: nspire-usbpad, a sibling repository like ../libretro-gpsp
# (its calculator library is built there and linked; its headers are only
# the glue's). Without it (USBPAD_DIR missing, or USBPAD=0) gpSP builds
# with no pads.
# The original sibling is frozen for archived gpSP builds whose shutdown
# code predates API 5. Prefer the tested current driver worktree when present.
USBPAD_DIR ?= $(if $(wildcard ../nspire-usbpad-pad/usbpad.h),../nspire-usbpad-pad,../nspire-usbpad)
USBPAD     ?= $(if $(wildcard $(USBPAD_DIR)/usbpad.h),1,0)
ifeq ($(USBPAD),1)
  USBPAD_LIB := usbpad_gpsp.o
  USBPAD_CFLAGS := -I$(USBPAD_DIR)
  # Keep the descriptor-based match fixes and the callback shutdown API:
  # otherwise the glue silently compiles out the wait that protects code
  # still executing in the OS task. The log's build id records the driver.
  USBPAD_NEED := USBPAD_OS_J_CLAIMED
  ifeq ($(filter clean,$(MAKECMDGOALS)),)
    ifeq ($(shell grep -l '$(USBPAD_NEED)' $(USBPAD_DIR)/src/backend_os.h 2>/dev/null),)
      $(error gpSP: the gamepad driver in $(USBPAD_DIR) lacks the match fix ($(USBPAD_NEED) not in src/backend_os.h): build with USBPAD_DIR=../nspire-usbpad-pad (branch match-fix, d74a333 or later), fast-forward $(USBPAD_DIR), or USBPAD=0 for no gamepads)
    endif
    ifeq ($(shell grep -l USBPAD_OS_HAS_SHUTDOWN_PENDING $(USBPAD_DIR)/src/backend_os.h 2>/dev/null),)
      $(error gpSP: the gamepad driver in $(USBPAD_DIR) lacks safe callback shutdown; use API 5 driver e234639 or later, set USBPAD_DIR=../nspire-usbpad-pad, or build USBPAD=0)
    endif
  endif
else
  USBPAD_LIB :=
  USBPAD_CFLAGS := -DNSPIRE_NO_USBPAD
  $(info gpSP: built without USB gamepads (no $(USBPAD_DIR)/usbpad.h))
endif

USBMS_DIR ?= ../nspire-usbms
USBMS ?= $(if $(wildcard $(USBMS_DIR)/usbms.h),1,0)
USBSHIM ?= ../nspire-usbshim
ifeq ($(USBMS),1)
  ifeq ($(filter clean,$(MAKECMDGOALS)),)
    ifeq ($(shell grep -l USBMS_PORT_OWNERSHIP_API $(USBMS_DIR)/usbms.h 2>/dev/null),)
      $(error gpSP: $(USBMS_DIR) needs USBMS_PORT_OWNERSHIP_API; update nspire-usbms, set USBMS_DIR to the tested drive branch, or build USBMS=0)
    endif
  endif
  DRIVE_OBJS := drive_msc.o drive_usbms.o drive_fs.o drive_unicode.o drive_os.o
  DRIVE_FLAGS := -I$(USBMS_DIR) -I$(USBSHIM)
else
  DRIVE_FLAGS := -DNSPIRE_NO_USBMS
endif

# Bluetooth (nspire-bt, a sibling like ../nspire-usbms): controllers and
# headphones through a USB Bluetooth dongle. Its library (the profiles,
# BTstack, the calculator port: build/nspire/libnspirebt.a, -Os with the
# SBC encoder -O2, function/data sections) is built there and linked;
# --gc-sections keeps what gpSP calls. It links no nspire-usbshim of its own
# (gpSP's one copy: usbpad_all.o or drive_shim.o). BT=0 (or no
# ../nspire-bt): no Bluetooth, -DNSPIRE_NO_BT.
BT_DIR ?= ../nspire-bt
BT ?= $(if $(wildcard $(BT_DIR)/include/nspire_bt.h),1,0)
# The library's options (nspire-bt profiles.mk; the size of each in gpsp.tns:
# docs/bluetooth.txt): BT_LE=full (the default since r19-bt: LE controllers
# connect, the Xbox Wireless Controller among them; its own ATT responder in
# place of BTstack's server), scan (a search lists LE devices, none
# connects), off (Classic only); BT_AVRCP=0 (the default: the headphones'
# volume in the samples, no absolute volume or their keys: -10.0 KB) or 1.
BT_LE ?= full
BT_AVRCP ?= 0
BT_OPTS := $(if $(filter off,$(BT_LE)),NBT_NO_LE=1,$(if $(filter scan,$(BT_LE)),NBT_LE_SCAN_ONLY=1)) \
           $(if $(filter 1,$(BT_AVRCP)),,NBT_NO_AVRCP=1)
BT_VARIANT := $(if $(filter off,$(BT_LE)),-nole,$(if $(filter scan,$(BT_LE)),-lescan))$(if $(filter 1,$(BT_AVRCP)),,-noavrcp)
ifeq ($(BT),1)
  BT_LIB := $(BT_DIR)/build/nspire/libnspirebt$(BT_VARIANT).a
  BT_FLAGS := -I$(BT_DIR)/include -I$(USBPAD_DIR)
else
  BT_LIB :=
  BT_FLAGS := -DNSPIRE_NO_BT
  $(info gpSP: built without Bluetooth (no $(BT_DIR)/include/nspire_bt.h))
endif

# USB Audio Class 1 output; the core keeps its silent timing path when no
# adapter is attached. Build USB_AUDIO=0 for the smallest silent binary.
# The driver's worktree kept for gpSP (../nspire-usbaudio-gpsp, branch
# gpsp/snd) when it is there, as USBPAD_DIR prefers ../nspire-usbpad-pad.
# GPSP_UA_NX: transfers of GPSP_UA_NF ms queued (2026-11-15: 7 of 8, 56 of the
# OS's 58 descriptors: 48 ms ahead of the bus after the OS ran; 2026-11-19,
# speed18: 4 of 14, the same 56 descriptors, 71 completions a second for 125,
# 42 ms ahead after the OS ran; UA_START_ONE: the driver's start queues the
# first alone, so its queue's clock is exact; nspire_audio.c).
USB_AUDIO_DIR ?= $(if $(wildcard ../nspire-usbaudio-gpsp/src/ua_os.h),../nspire-usbaudio-gpsp,../nspire-usbaudio)
USB_AUDIO ?= $(if $(wildcard $(USB_AUDIO_DIR)/src/ua_os.h),1,0)
GPSP_UA_NX ?= 4
GPSP_UA_NF ?= 14
ifeq ($(USB_AUDIO),1)
  ifeq ($(filter clean,$(MAKECMDGOALS)),)
    ifeq ($(shell grep -l ua_owns_usb $(USB_AUDIO_DIR)/src/ua_os.h 2>/dev/null),)
      $(error gpSP: update nspire-usbaudio with safe discard/ownership support, or build USB_AUDIO=0)
    endif
  endif
  CFLAGS := $(filter-out -DNSPIRE_NO_AUDIO,$(CFLAGS)) -DNSPIRE_RUNTIME_AUDIO
  CXXFLAGS := $(filter-out -DNSPIRE_NO_AUDIO,$(CXXFLAGS)) -DNSPIRE_RUNTIME_AUDIO
  # RING_FRAMES (bt10: 8192 -> 16384, +32 KB of .bss): with Bluetooth headphones playing beside the adapter,
  # its sound is held back to theirs (~220 ms: nspire_audio.c sync_with_headphones); the ring holds 341 ms at
  # 48 kHz, its target 3/4 of it at the most
  AUDIO_FLAGS := -I$(USB_AUDIO_DIR)/src -I$(USBSHIM) -DRING_FRAMES=16384 -DUA_NX=$(GPSP_UA_NX) -DUA_NF=$(GPSP_UA_NF) -DUA_START_ONE
  AUDIO_OBJS := audio_pcm.o audio_uac.o audio_os.o
endif
# usbpad_all.o already contains the shared driver registry. With pads off,
# drive, audio and Bluetooth link exactly one copy between them.
ifneq ($(USBPAD),1)
  ifneq ($(filter 1,$(USBMS) $(USB_AUDIO) $(BT)),)
    SHIM_OBJS := drive_shim.o
  endif
endif

LOCAL_OBJS := \
  nspire_host_main.o nspire_frameskip.o nspire_video_present.o nspire_osd.o nspire_video_ui.o \
  nspire_input.o nspire_stubs.o nspire_backup.o nspire_stack_glue.o \
  nspire_render_time.o nspire_lines.o video_cc_renderer.o \
  upscale_aspect.o scanline_fill16.o blend_landscape.o blend_portrait.o nspire_upscale_from_gba.o nspire_turn.o nspire.o \
  nspire_reuse.o nspire_pictures.o \
  nspire_gbz.o nspire_states.o nspire_fastdeflate.o nspire_config.o nspire_clock.o nspire_sram.o clock_switch_nspire.o nspire_keys.o \
  nspire_brightness.o nspire_game_list.o nspire_blend.o nspire_link.o nspire_link_usb.o nspire_link_fast.o nspire_link_speed.o nspire_os.o \
  nspire_zip.o nspire_loading.o nspire_save_manager.o nspire_paths.o nspire_fswrap.o \
  nspire_selfstore.o nspire_rom_convert.o nspire_rom_manager.o nspire_ui.o nspire_ui_screens.o nspire_controls.o nspire_bind.o nspire_dialog.o \
  nspire_tickrate.o nspire_usbpad_glue.o nspire_usbport.o nspire_arbiter.o nspire_usbcycle.o nspire_exit.o \
  nspire_padstatus.o nspire_padlog.o nspire_padpage.o nspire_drive.o nspire_drive_page.o nspire_audio.o nspire_audio_page.o nspire_cheats.o \
  nspire_devices.o nspire_drive_saves.o nspire_bt_glue.o nspire_bt_page.o \
  newlib_qsort.o newlib_strstr.o

# The game's code and data, pinned (tools/pin_layout.py;
# nspire_lineio.ld). Only the game's files are LTO's (cross-file inlining,
# section anchors), linked first: LTO's partitions, so the order of its data,
# depend on them alone. nspire_lineio.ld puts every function a game runs at
# its place in a 4 KB way and the data in one block at a fixed offset.
# The frontend (menus, files, link, sound, pads, settings) is built without
# LTO, its sections after the game's: an edit there moves nothing of the
# game's (tests/layout_pinned.py). It is -O2, its cold part -Os, a few files
# -O3 (nspire_cold.mk COLD_C, FRONT_O3_C). A new frontend file needs nothing
# (it's FRONT_OBJS, -O2); a new file of the game goes into GAME_OBJS.
GAME_OBJS := main.o gba_memory.o sound.o cheats.o memmap.o serial.o gbp.o rfu.o serial_proto.o \
  serial_fe.o serial_msio.o gba_cc_lut.o cpu_threaded.o cpu.o video_cc_renderer.o \
  nspire_video_present.o nspire_lines.o nspire_render_time.o nspire_upscale_from_gba.o nspire_turn.o \
  nspire_blend.o nspire_stubs.o nspire.o
ASM_OBJS := bios_data.o arm_stub.o upscale_aspect.o scanline_fill16.o blend_landscape.o blend_portrait.o \
  clock_switch_nspire.o nspire_stack_glue.o zehn_entry.o
include nspire_cold.mk
LDFLAGS += $(PRINTF_LDFLAGS)
COLD_OBJS := $(addsuffix .o,$(COLD_C))
# zlib built from the SDK's sources (nspire_cold.mk ZLIB_C): libz.a's code,
# function by function, without what nothing calls and crc32's 8 KB table.
# The SDK's libz.a (the linker adds -lz) when its sources aren't there.
ZLIB_DIR := $(shell dirname "$$(command -v $(GCC))")/../thirdparty/zlib
ZLIB_OBJS := $(if $(wildcard $(ZLIB_DIR)/crc32.c),$(addprefix zlib_,$(addsuffix .o,$(ZLIB_C))))
FRONT_OBJS := $(filter-out $(GAME_OBJS) $(ASM_OBJS) $(COLD_OBJS),$(COMM_OBJS) $(CORE_OBJS) $(LOCAL_OBJS) gui.o)
OBJS := $(GAME_OBJS) $(ASM_OBJS) $(FRONT_OBJS) $(COLD_OBJS)
# -ffunction-sections: --gc-sections drops what nothing calls (LTO did that
# before); the cold part's data too (-fdata-sections: no section anchors,
# nothing there is hot). -O2: r16's shrink (-O3's inlining
# and loop copies made those files' code 45 KB bigger, nothing they run
# measurably faster).
FRONT_CFLAGS := $(filter-out -flto -O3,$(CFLAGS)) -ffunction-sections
$(FRONT_OBJS) $(DRIVE_OBJS) audio_pcm.o audio_uac.o audio_os.o: CFLAGS := $(FRONT_CFLAGS) -O2
$(addsuffix .o,$(FRONT_O3_C)): CFLAGS := $(FRONT_CFLAGS) -O3
$(COLD_OBJS) $(addsuffix .o,$(COLD_DRIVERS)): CFLAGS := $(FRONT_CFLAGS) -Os -fdata-sections
$(addsuffix .o,$(FRONT_DATA_SECTIONS_C)): CFLAGS += -fdata-sections

# Same flags as CXXFLAGS but without LTO so -S yields readable per-TU assembly.
CXXFLAGS_NO_LTO := $(filter-out -flto,$(CXXFLAGS))

.PHONY: all clean video-cc-asm FORCE

all: gpsp.tns

# Switching a feature or a driver worktree must rebuild the affected ABI,
# including sound's runtime-only fields. A clean build is not required.
BUILD_CFLAGS := $(CFLAGS)
BUILD_CXXFLAGS := $(CXXFLAGS)
.gpsp-build-flags: FORCE
	@printf '%s\n' "$(BUILD_CFLAGS)" "$(BUILD_CXXFLAGS)" "$(USBPAD_DIR) $(USBPAD) $(USBMS_DIR) $(USBMS) $(USBSHIM) $(BT_DIR) $(BT)" "$(USBPAD_CFLAGS) $(DRIVE_FLAGS) $(AUDIO_FLAGS) $(BT_FLAGS)" "$(GAME_OBJS) | $(COLD_OBJS) $(COLD_DRIVERS) | $(FRONT_O3_C) | $(FRONT_DATA_SECTIONS_C)" "$(ZLIB_DIR) $(ZLIB_OBJS) $(ZLIB_FLAGS)" > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

# Inspect video.cc codegen (ARM926EJ-S): docker compose run --rm ndless-dev make video-cc-asm
video-cc-asm: $(CORE_DIR)/video.cc
	$(GXX) $(CXXFLAGS_NO_LTO) -S -fverbose-asm -o video.cc.asm $<

nspire_usbpad_glue.o: CFLAGS += $(USBPAD_CFLAGS)
nspire_bt_glue.o nspire_bt_page.o: CFLAGS += $(BT_FLAGS)
nspire_drive.o: CFLAGS += $(DRIVE_FLAGS)
nspire_audio.o: CFLAGS += $(AUDIO_FLAGS)

audio_pcm.o: $(USB_AUDIO_DIR)/src/pcm.c
	$(GCC) $(CFLAGS) $(AUDIO_FLAGS) -c $< -o $@
audio_uac.o: $(USB_AUDIO_DIR)/src/uac.c
	$(GCC) $(CFLAGS) $(AUDIO_FLAGS) -c $< -o $@
# ua_os.c through gpSP's wrapper: it leaves the PlayStation pads' audio
# interfaces to the gamepad driver (nspire_audio_os.c, 2026-11-15).
audio_os.o: nspire_audio_os.c $(USB_AUDIO_DIR)/src/ua_os.c
	$(GCC) $(CFLAGS) $(AUDIO_FLAGS) -c $< -o $@

# (-Os, 2026-11-17: with the drive written to, msc.c and usbms.c were 11.5 KB
# more at -O3, its retry loops inlined into every command; each command
# waits on the drive, milliseconds)
drive_msc.o: $(USBMS_DIR)/msc.c
	$(GCC) $(CFLAGS) $(DRIVE_FLAGS) -Os -c $< -o $@
drive_usbms.o: $(USBMS_DIR)/usbms.c
	$(GCC) $(CFLAGS) $(DRIVE_FLAGS) -Os -c $< -o $@
drive_fs.o: $(USBMS_DIR)/fatfs/ff.c
	$(GCC) $(CFLAGS) $(DRIVE_FLAGS) -Os -c $< -o $@
drive_unicode.o: $(USBMS_DIR)/fatfs/ffunicode.c
	$(GCC) $(CFLAGS) $(DRIVE_FLAGS) -Os -c $< -o $@
# usbms_nspire.c through gpSP's wrapper: its attach never returns an error
# after its match said yes (TI's OS then offers nothing more: R11u,
# nspire_drive_os.c, 2026-11-15).
drive_os.o: nspire_drive_os.c $(USBMS_DIR)/usbms_nspire.c
	$(GCC) $(CFLAGS) $(DRIVE_FLAGS) -c $< -o $@
drive_shim.o: $(USBSHIM)/usbshim.c
	$(GCC) $(CFLAGS) -I$(USBSHIM) -c $< -o $@

zlib_%.o: $(ZLIB_DIR)/%.c
	$(GCC) $(ZLIB_FLAGS) -c $< -o $@

# The build id the link log leads with (tools/build_id.sh): git describe,
# date, the gamepad driver's. Rewritten only when the id changes.
nspire_build_id.h: FORCE
	@sh tools/build_id.sh $@ $(USBPAD_DIR) $(USBPAD) .
nspire_link.o nspire_padlog.o nspire_ui_screens.o: nspire_build_id.h

# Bluetooth's library, by its own Makefile (nothing to do, usually), with
# gpSP's shim and pad headers.
ifeq ($(BT),1)
$(BT_LIB): FORCE
	$(MAKE) -C $(BT_DIR) lib $(BT_OPTS) USBSHIM=$(abspath $(USBSHIM)) USBPAD_DIR=$(abspath $(USBPAD_DIR))
endif

# Its own Makefile says when the library is out of date (nothing to do,
# usually: the link then needn't run again).
$(USBPAD_DIR)/build/nspire/libusbpad.a: FORCE
	$(MAKE) -C $(USBPAD_DIR) build/nspire/libusbpad.a

# gpSP links a copy of the library's one object (libusbpad.a holds just
# usbpad_all.o) whose nspire_usbpad_active is renamed usbpad_lib_active:
# nspire_link.c has a weak default of it (return 0), and GCC 14.2's LTO
# (with gold) inlines that default even when the linker resolves the name
# to a regular object's strong one (the library's). So the strong one is
# the glue's, in LTO code, and it calls the library's by the new name.
usbpad_gpsp.o: $(USBPAD_DIR)/build/nspire/libusbpad.a
	$(AR) p $< usbpad_all.o > $@
	$(OBJCOPY) --redefine-sym nspire_usbpad_active=usbpad_lib_active $@

gui.o: gui.c
	$(GCC) $(CFLAGS) $(GUI_CPPFLAGS) -c $< -o $@

nspire.o: nspire/nspire.c
	$(GCC) $(CFLAGS) -c $< -o $@

upscale_aspect.o: nspire/upscale_aspect.s
	$(GCC) $(CFLAGS) -c $< -o $@

scanline_fill16.o: nspire/scanline_fill16.S
	$(GCC) $(CFLAGS) -c $< -o $@

blend_landscape.o: nspire/blend_landscape.S
	$(GCC) $(CFLAGS) -c $< -o $@

blend_portrait.o: nspire/blend_portrait.S
	$(GCC) $(CFLAGS) -c $< -o $@

clock_switch_nspire.o: nspire/clock_switch_nspire.S
	$(GCC) $(CFLAGS) -c $< -o $@

zehn_entry.o: nspire/zehn_entry.S
	$(GCC) $(CFLAGS) -c $< -o $@

cpu.o: $(CORE_DIR)/cpu.cc
	$(GXX) $(CXXFLAGS) -c $< -o $@

video_cc_renderer.o: $(CORE_DIR)/video.cc
	$(GXX) $(CXXFLAGS) \
	-Dupdate_scanline=video_cc_update_scanline \
	-c $< -o $@

%.o: %.c
	$(GCC) $(CFLAGS) -c $< -o $@

%.o: %.cc
	$(GXX) $(CXXFLAGS) -c $< -o $@

%.o: %.S
	$(GCC) $(CFLAGS) -c $< -o $@

%.o: %.s
	$(GCC) $(CFLAGS) -c $< -o $@

gpsp_libretro.elf: $(OBJS) $(USBPAD_LIB) $(DRIVE_OBJS) $(AUDIO_OBJS) $(SHIM_OBJS) $(BT_LIB) $(ZLIB_OBJS) $(LINEIO_LD)
	$(GXX) $(LDFLAGS) $(filter-out $(LINEIO_LD),$^) -o $@ $(LDLIBS)

gpsp_libretro.tns: gpsp_libretro.elf tools/zehn_pack.py
ifeq ($(ZEHN_PACK),1)
	$(GENZ) $(ZEHNFLAGS) --input $< --output gpsp_libretro.zehn
	python3 -I tools/zehn_pack.py gpsp_libretro.zehn $< $@
else
	$(GENZ) $(ZEHNFLAGS) --compress --input $< --output $@
endif

gpsp.tns: gpsp_libretro.tns
	cp $< $@

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) gpsp_libretro.elf gpsp_libretro.tns gpsp_libretro.zehn gpsp_libretro.map gpsp.tns video.cc.asm usbpad_gpsp.o drive_*.o drive_*.d audio_*.o audio_*.d zlib_*.o

# Header dependencies (-MMD): a changed header (arm_emit.h...) rebuilds what
# includes it.
-include $(OBJS:.o=.d)
-include $(DRIVE_OBJS:.o=.d)

-include $(AUDIO_OBJS:.o=.d) $(SHIM_OBJS:.o=.d)

$(OBJS) $(USBPAD_LIB) $(DRIVE_OBJS) $(AUDIO_OBJS) $(SHIM_OBJS) $(ZLIB_OBJS): .gpsp-build-flags
