# PC tests (run.sh runs them all):
#   core_tests      the core with its interpreter (the dynarec is ARM only):
#                   compressed ROMs, save states, in-game saves
#   frontend_tests  frame turning, key bindings, the settings files
#   gpsp-host       all of gpSP's frontend (menus, file list, game loop) on a
#                   simulated calculator (host_hw.c), driven by scripts, with
#                   a stand-in for the USB gamepad driver (host_usbpad.c)
#   ui_preview      the menus' screens with real games' pictures, to look at
#                   (ui_preview.c)

ROOT  := ../..
CORE  := $(ROOT)/libretro-gpsp
COMM  := $(CORE)/libretro/libretro-common
BUILD := build
# make SAN=1: the same programs in build-san with AddressSanitizer and
# UBSan (alignment included), stopping at the first report: what the
# calculator punishes (alignment exceptions on) and the PC forgives.
# tests/san.sh runs every suite with them.
SAN ?=
ifneq ($(SAN),)
  BUILD := build-san
  SANFLAGS := -fsanitize=address,undefined,alignment -fno-sanitize-recover=all -fno-omit-frame-pointer
  # gpSP's own core (upstream code, not the link's stand-ins) shifts into the
  # sign bit by design: everything else stays on for it.
  CORE_SAN := -fno-sanitize=shift
endif
# The serial port and the link's stand-ins are ours: every check.
OURS_C := serial gbp rfu serial_proto serial_fe serial_msio

# NSPIRE_BACKUP_CHECK: nspire_backup.c checks that a save it doesn't write
# when the game is left is the one last loaded or written.
DEFS  := -DNSPIRE_BUILD -DNSPIRE_LIBRETRO -DNSPIRE_NO_AUDIO -DNSPIRE_HOST -DROM_BUFFER_SIZE=8 \
         -DNSPIRE_BACKUP_CHECK
INCS  := -Iinclude -I$(CORE) -I$(CORE)/libretro -I$(COMM)/include -I$(ROOT) -I$(ROOT)/nspire
# The core and gpSP's old menu code have many warnings; they stay on for the
# new frontend files.
# -MMD: a changed header rebuilds what includes it (R6: stale objects).
CORE_FLAGS  := -O1 -g -w -MMD -MP $(SANFLAGS) $(DEFS) $(INCS)
FRONT_FLAGS := -O1 -g -std=gnu99 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
               -Wno-old-style-declaration -MMD -MP $(SANFLAGS) $(DEFS) $(INCS)
OLD_FLAGS   := -O1 -g -std=gnu99 -w -MMD -MP $(SANFLAGS) $(DEFS) $(INCS)

# USB gamepads: nspire-usbpad's headers, for the glue and host_usbpad.c
# (its stand-in for the driver); without them, the glue has no pads.
USBPAD_DIR ?= $(if $(wildcard $(ROOT)/../nspire-usbpad-pad/usbpad.h),$(ROOT)/../nspire-usbpad-pad,$(ROOT)/../nspire-usbpad)
ifneq ($(wildcard $(USBPAD_DIR)/usbpad.h),)
  PAD_FLAGS := -I$(USBPAD_DIR)
else
  PAD_FLAGS := -DNSPIRE_NO_USBPAD
endif

# Bluetooth: nspire-bt's headers (nspire_bt.h, bt_port.h) for the glue, the
# page and host_bt.c (the stand-in for the library and the port).
BT_DIR ?= $(ROOT)/../nspire-bt
ifneq ($(wildcard $(BT_DIR)/include/nspire_bt.h),)
  BT_FLAGS := -I$(BT_DIR)/include -I$(USBPAD_DIR)
  BT_HOST_OBJS := $(BUILD)/test_host_bt.o
else
  BT_FLAGS := -DNSPIRE_NO_BT
  BT_HOST_OBJS :=
endif

USBMS_DIR ?= $(ROOT)/../nspire-usbms
ifneq ($(wildcard $(USBMS_DIR)/usbms.h),)
  DRIVE_FLAGS := -I$(USBMS_DIR)
  DRIVE_OBJS := $(BUILD)/drive_msc.o $(BUILD)/drive_usbms.o $(BUILD)/drive_fs.o \
                $(BUILD)/drive_unicode.o $(BUILD)/drive_fake.o
else
  DRIVE_FLAGS := -DNSPIRE_NO_USBMS
endif
# R11u: drive_os_tests (below), with the drive driver's TI layer and the shim
SHIM_DIR ?= $(ROOT)/../nspire-usbshim
ifneq ($(wildcard $(USBMS_DIR)/usbms_nspire.c),)
  DRIVE_OS_TESTS := $(BUILD)/drive_os_tests
endif

CORE_C   := main gba_memory savestate sound cheats memmap serial gbp rfu serial_proto serial_fe serial_msio gba_cc_lut
COMM_C   := compat/compat_posix_string compat/compat_strl compat/fopen_utf8 encodings/encoding_utf \
            file/file_path file/file_path_io streams/file_stream string/stdstring time/rtime \
            vfs/vfs_implementation

CORE_OBJS := $(addprefix $(BUILD)/core_,$(addsuffix .o,$(CORE_C))) $(BUILD)/core_cpu.o \
             $(addprefix $(BUILD)/comm_,$(addsuffix .o,$(notdir $(COMM_C)))) $(BUILD)/bios_data.o

# The unit tests: the parts that don't touch the calculator, and stubs.
UNIT_FRONT := nspire_gbz nspire_stubs nspire_backup nspire_states nspire_fastdeflate nspire_ui nspire_turn nspire_keys nspire_bind nspire_config nspire_lines nspire_reuse \
              nspire_brightness nspire_game_list nspire_blend nspire_zip nspire_paths nspire_sram \
              nspire_selfstore nspire_rom_convert nspire_link nspire_link_speed nspire_tickrate nspire_usbport nspire_arbiter \
              nspire_padstatus nspire_padlog nspire_osd
UNIT_OBJS  := $(CORE_OBJS) $(BUILD)/core_video.o \
              $(addprefix $(BUILD)/front_,$(addsuffix .o,$(UNIT_FRONT))) $(BUILD)/test_host_stubs.o \
              $(BUILD)/unit_bt_glue.o

# gpsp-host: the whole frontend, with video.cc's update_scanline renamed as on the calculator.
HOST_FRONT := nspire_gbz nspire_stubs nspire_backup nspire_states nspire_fastdeflate nspire_ui nspire_ui_screens nspire_turn nspire_keys nspire_config \
              nspire_input nspire_controls nspire_bind nspire_dialog nspire_video_ui nspire_video_present nspire_pictures nspire_osd nspire_frameskip \
              nspire_render_time nspire_lines nspire_reuse nspire_upscale_from_gba nspire_brightness nspire_sram \
              nspire_game_list nspire_blend nspire_zip nspire_loading nspire_save_manager \
              nspire_paths nspire_selfstore nspire_rom_convert nspire_rom_manager nspire_link nspire_link_speed nspire_tickrate \
              nspire_usbpad_glue nspire_usbport nspire_arbiter nspire_usbcycle nspire_exit \
              nspire_padstatus nspire_padlog nspire_padpage nspire_drive nspire_drive_page nspire_audio nspire_audio_page nspire_devices nspire_drive_saves nspire_cheats \
              nspire_bt_glue nspire_bt_page
HOST_OBJS  := $(CORE_OBJS) $(BUILD)/video_cc_renderer.o \
              $(addprefix $(BUILD)/front_,$(addsuffix .o,$(HOST_FRONT))) \
              $(BUILD)/host_main.o $(BUILD)/host_gui.o $(BUILD)/host_nspire.o \
              $(BUILD)/test_host_hw.o $(BUILD)/test_host_platform.o $(BUILD)/test_host_link.o \
              $(BUILD)/test_host_usbpad.o $(DRIVE_OBJS) $(BT_HOST_OBJS)

all: $(BUILD)/core_tests $(BUILD)/frontend_tests $(BUILD)/gpsp-host $(BUILD)/ui_preview $(BUILD)/os_tests $(BUILD)/memmap_tests $(BUILD)/file_replace_tests \
     $(DRIVE_OS_TESTS)

$(BUILD):
	mkdir -p $@

$(BUILD)/core_%.o: $(CORE)/%.c | $(BUILD)
	gcc $(CORE_FLAGS) $(if $(filter $*,$(OURS_C)),,$(CORE_SAN)) -c $< -o $@
$(BUILD)/core_%.o: $(CORE)/%.cc | $(BUILD)
	g++ $(CORE_FLAGS) $(CORE_SAN) -std=gnu++11 -c $< -o $@

define comm_rule
$(BUILD)/comm_$(notdir $(1)).o: $(COMM)/$(1).c | $(BUILD)
	gcc $(CORE_FLAGS) $(CORE_SAN) -c $$< -o $$@
endef
$(foreach f,$(COMM_C),$(eval $(call comm_rule,$(f))))

$(BUILD)/bios_data.o: $(CORE)/bios_data.S | $(BUILD)
	gcc -c $< -I$(CORE) -o $@

$(BUILD)/video_cc_renderer.o: $(CORE)/video.cc | $(BUILD)
	g++ $(CORE_FLAGS) $(CORE_SAN) -std=gnu++11 \
	-Dupdate_scanline=video_cc_update_scanline \
	-c $< -o $@

$(BUILD)/front_%.o: $(ROOT)/%.c | $(BUILD)
	gcc $(FRONT_FLAGS) -c $< -o $@
# nspire_usbcycle.c's power cycle for frontend_tests, renamed: the arbiter's tests
# keep host_stubs.c's stand-in, the cycle's own call the real one on its registers.
$(BUILD)/unit_usbcycle.o: $(ROOT)/nspire_usbcycle.c | $(BUILD)
	gcc $(FRONT_FLAGS) -Dnspire_usb_reenumerate=nspire_usb_reenumerate_real \
	-Dnspire_usb_power_restore=nspire_usb_power_restore_real -Dnspire_usb_power_restore_pending=nspire_usb_power_restore_pending_real -Dnspire_link_log=unit_cycle_log -c $< -o $@
# Bluetooth's glue without Bluetooth (its calls do nothing) for the unit
# tests and the models that link the pad glue, the sound or the pacing
# (padsim, sink, pace); gpsp-host has the real glue on host_bt.c.
$(BUILD)/unit_bt_glue.o: $(ROOT)/nspire_bt_glue.c | $(BUILD)
	gcc $(FRONT_FLAGS) -DNSPIRE_NO_BT -c $< -o $@
$(BUILD)/host_main.o: $(ROOT)/nspire_host_main.c | $(BUILD)
	gcc $(FRONT_FLAGS) -Dmain=gpsp_main -c $< -o $@
$(BUILD)/host_gui.o: $(ROOT)/gui.c | $(BUILD)
	gcc $(OLD_FLAGS) -DCOMPILING_GUI_MODULE -c $< -o $@
$(BUILD)/host_nspire.o: $(ROOT)/nspire/nspire.c | $(BUILD)
	gcc $(FRONT_FLAGS) -c $< -o $@
$(BUILD)/test_%.o: %.c | $(BUILD)
	gcc $(FRONT_FLAGS) -c $< -o $@
# The link log's build id header (tools/build_id.sh), as the calculator's.
$(ROOT)/nspire_build_id.h: FORCE
	@sh $(ROOT)/tools/build_id.sh $@ $(USBPAD_DIR) $(if $(PAD_FLAGS:-DNSPIRE_NO_USBPAD=),1,0) $(ROOT)
$(BUILD)/front_nspire_link.o $(BUILD)/front_nspire_padlog.o $(BUILD)/front_nspire_ui_screens.o: $(ROOT)/nspire_build_id.h
.PHONY: FORCE
FORCE:

$(BUILD)/front_nspire_usbpad_glue.o $(BUILD)/test_host_usbpad.o: FRONT_FLAGS += $(PAD_FLAGS)
$(BUILD)/front_nspire_bt_glue.o $(BUILD)/front_nspire_bt_page.o $(BUILD)/test_host_bt.o: FRONT_FLAGS += $(BT_FLAGS)
$(BUILD)/front_nspire_drive.o: FRONT_FLAGS += $(DRIVE_FLAGS)

$(BUILD)/drive_msc.o: $(USBMS_DIR)/msc.c | $(BUILD)
	gcc $(FRONT_FLAGS) $(DRIVE_FLAGS) -c $< -o $@
$(BUILD)/drive_usbms.o: $(USBMS_DIR)/usbms.c | $(BUILD)
	gcc $(FRONT_FLAGS) $(DRIVE_FLAGS) -c $< -o $@
$(BUILD)/drive_fs.o: $(USBMS_DIR)/fatfs/ff.c | $(BUILD)
	gcc $(FRONT_FLAGS) $(DRIVE_FLAGS) -c $< -o $@
$(BUILD)/drive_unicode.o: $(USBMS_DIR)/fatfs/ffunicode.c | $(BUILD)
	gcc $(FRONT_FLAGS) $(DRIVE_FLAGS) -c $< -o $@
$(BUILD)/drive_fake.o: $(USBMS_DIR)/tests/fake_drive.c | $(BUILD)
	gcc $(FRONT_FLAGS) $(DRIVE_FLAGS) -c $< -o $@

$(BUILD)/core_tests: $(UNIT_OBJS) $(BUILD)/test_core_tests.o
	g++ $(SANFLAGS) $^ -o $@ -lz

# R11u (2026-11-15): gpSP's build of the drive driver's TI layer
# (nspire_drive_os.c: usbms_nspire.c with its attach wrapped, never an error
# after its match said yes) on a fake of the OS calls it makes.
$(BUILD)/drive_os_tests: drive_os_tests.c $(ROOT)/nspire_drive_os.c $(USBMS_DIR)/usbms_nspire.c | $(BUILD)
	gcc -O1 -g -std=gnu99 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare $(SANFLAGS) -DUSBMS_OS_TEST \
	-I$(USBMS_DIR) -I$(SHIM_DIR) drive_os_tests.c $(ROOT)/nspire_drive_os.c -o $@

$(BUILD)/test_drive_tests.o: FRONT_FLAGS += $(DRIVE_FLAGS)
$(BUILD)/drive_tests: $(UNIT_OBJS) $(BUILD)/front_nspire_drive.o $(BUILD)/front_nspire_drive_saves.o $(DRIVE_OBJS) $(BUILD)/test_drive_tests.o
	g++ $(SANFLAGS) $^ -o $@ -lz -Wl,--wrap=malloc

$(BUILD)/frontend_tests: $(UNIT_OBJS) $(BUILD)/unit_usbcycle.o $(BUILD)/test_frontend_tests.o
	g++ $(SANFLAGS) $^ -o $@ -lz

# EXIT-R3: nspire_os.c itself (gpsp-host has stand-ins for it), its
# registers in os_tests.c's register file.
$(BUILD)/os_nspire_os.o: $(ROOT)/nspire_os.c | $(BUILD)
	gcc $(FRONT_FLAGS) -Ios_include -DNSPIRE_OS_FAKE_REGS -c $< -o $@
$(BUILD)/os_tests: $(BUILD)/os_nspire_os.o $(BUILD)/test_os_tests.o
	gcc $(SANFLAGS) $^ -o $@

$(BUILD)/ui_preview: $(BUILD)/test_ui_preview.o $(BUILD)/front_nspire_ui.o $(BUILD)/front_nspire_ui_screens.o
	gcc $(SANFLAGS) $^ -o $@

# gui.c's menus are GCC nested functions, whose trampolines need an
# executable stack. The wrapped functions cost virtual time (host_hw.c), or
# take gpsp-host's --link-device (nspire_link_apply_device), or
# GPSP_HOST_CLOCK (time), or --rom-memory, --free-storage, --heap-limit and
# --max-files (malloc, calloc, realloc, free, fopen, fclose, fwrite, remove,
# opendir, closedir), or take the OS's paths with --os-cwd (fopen, stat,
# opendir, chdir, rename, remove, mkdir, rmdir, open), or go into
# out/pad.txt too (nspire_osd_message: host_usbpad.c), or say the buffer a
# log was written with (setvbuf: the report's log_buffer lines).
HOST_WRAP := execute_arm update_serial update_scanline nspire_turn_raw nspire_turn_1x nspire_turn_rect_555 \
             nspire_turn_rect_565 nspire_blend_turn_aspect nspire_blend_turn_full nspire_present_frame \
             nspire_blend_aspect_landscape upscale_aspect upscale_aspect_fast upscale_aspect_raw upscale_aspect_raw_from_gba \
             filestream_read zip_read nspire_link_apply_device time malloc calloc realloc free fopen fclose fwrite remove nspire_osd_message nspire_usb_reenumerate \
             setvbuf usbms_platform_stop nspire_bt_stop_within \
             stat opendir closedir chdir rename mkdir rmdir open \
             serialpoke_net_receive serialaw_net_receive rfu_net_receive serialfe_net_receive serialmsio_net_receive
$(BUILD)/gpsp-host: $(HOST_OBJS)
	g++ $(SANFLAGS) $^ -o $@ -lz -lm -z execstack $(addprefix -Wl$(comma)--wrap=,$(HOST_WRAP))

comma := ,

clean:
	rm -rf $(BUILD) build-sink build-padsim

.PHONY: all clean

-include $(wildcard $(BUILD)/*.d)

# Optional desktop allocator, tested without making real executable mappings.
$(BUILD)/memmap_tests: memmap_tests.c $(CORE)/memmap.c $(CORE)/memmap.h | $(BUILD)
	gcc -O1 -g -Wall -Wextra $(SANFLAGS) $< -o $@

# Filesystem fault tests, retaining only the replacement helper at link.
$(BUILD)/file_replace_tests: file_replace_tests.c $(ROOT)/nspire_paths.c $(ROOT)/nspire_paths.h | $(BUILD)
	gcc $(FRONT_FLAGS) -ffunction-sections -fdata-sections $< -Wl,--gc-sections -o $@

# Compression allocation failure injection is confined to host tests.
$(BUILD)/front_nspire_fastdeflate.o: FRONT_FLAGS += -DNSPIRE_ALLOC_HOOK
$(BUILD)/front_nspire_states.o: FRONT_FLAGS += -DNSPIRE_ALLOC_HOOK

# The simulated calculator's own hot code (its timers and LCD, polled
# hundreds of millions of times a run) at -O2: the tests' time, not gpSP's.
$(BUILD)/test_host_hw.o $(BUILD)/test_host_platform.o: FRONT_FLAGS += -O2
