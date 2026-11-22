# Tests of gpSP's ARM-only code, built for the calculator's CPU and run
# under qemu-arm (tests/arm/run.sh builds and runs them):
#   blend_test    the one-pass blended pictures (nspire_blend.c, and the 3:2
#                 ones' assembly: nspire/blend_portrait.S, the CX II's, and
#                 nspire/blend_landscape.S, the older CX's) against the old
#                 path (notaz's scalers in nspire/upscale_aspect.s)
#   fill_test     video.cc's backdrop fill (nspire/scanline_fill16.S) at
#                 every start pixel: it crashed on odd ones
#   dynarec_test  the core with its ARM dynarec on rom/ram_code.S: code in
#                 RAM branching between its blocks and rewritten, with either
#                 "Dynarec RAM / SMC" setting
#   badjump_test  the core on rom/bad_jump.S, which jumps where no code can
#                 run (as a damaged Advance Wars does): it has to stop there
#   dispatch_test arm_stub.S's mem_dispatch_* stubs: every top byte and low
#                 bits reach the handler the old 17-entry clamp gave
#   block_check_test arm_stub.S's nspire_block_check{,_tag}: the fast
#                 push/pop/ldm/stm check decides as the old inline one did
#   idle_test     the core on rom/idle_loop.S: a pure idle loop (core-16)
#                 sees every line of an I/O register and a DMA's write
#   replay        the core playing a real game from a save state (run.sh
#                 runs it on Pokemon Emerald when $GBA_GAMES has it)
#   perf          replay plus the picture output, for the speed estimate
#                 (tests/calc_speed.sh runs it under the ARM926 model)
#   ui_bench      what the menus' drawing costs (ui_bench.sh runs it under
#                 the ARM926 model)
#   fastdeflate_test  nspire_fastdeflate.c's zlib streams (states and their
#                 pictures) unpacked by zlib, within their bound
#   zehn_test     the packed program file (tools/zehn_pack.py) loads as
#                 genzehn's own, its entry's relocations run from the image
#   libbench      gpSP's load and save paths (ROMs, states, saves, settings,
#                 the file list) over the calculator's libsyscalls and a
#                 stand-in for its OS (libbench_os.c); tests/libbench.sh
#                 runs it under the ARM926 model
#   printf_test   newlib's printf family as the calculator build links it
#                 (one integer stream printf for all of it) gives what
#                 newlib's own does
# The calculator's files are compiled as for the calculator (nspire-gcc,
# -O3, without -flto so they link here); the core is compiled with the
# calculator's flags by arm-none-eabi-gcc with the PC tests' stand-in os.h
# (Ndless's headers clash with it). The test programs use newlib's
# semihosting library. Needs the Ndless toolchain on PATH.

ROOT  := ../..
CORE  := $(ROOT)/libretro-gpsp
COMM  := $(CORE)/libretro/libretro-common
BUILD := build$(if $(AUDIO),/audio)
ZLIB  := $(shell dirname $$(command -v nspire-gcc))/../thirdparty/zlib
LIBZ  := $(shell dirname $$(command -v nspire-gcc))/../lib/libz.a

ARCH  := -marm -mcpu=arm926ej-s
CALC_FLAGS := -O3 -std=c99 -msoft-float -funsigned-char -fno-common $(ARCH) -fomit-frame-pointer \
              -DNSPIRE_BUILD -DNSPIRE_LIBRETRO -DNSPIRE_NO_AUDIO -DARM_ARCH \
              -I$(CORE) -I$(CORE)/libretro -I$(CORE)/libretro/libretro-common/include \
              -I$(ROOT) -I$(ROOT)/nspire -Wall -MMD -MP
TEST_FLAGS := -O2 $(ARCH) -Wall -MMD -MP

# The core as the calculator's Makefile builds it.
# NSPIRE_LAYOUT_MIRROR (common.h): no .text.hot.gpsp section; perf's link
# puts every function where the calculator build has it (mirror_layout.py).
CORE_DEFS  := -DNSPIRE_BUILD -DNSPIRE_LIBRETRO $(if $(AUDIO),-DPERF_AUDIO,-DNSPIRE_NO_AUDIO) -DNSPIRE_HOST -DARM_ARCH \
              -DHAVE_DYNAREC=1 -DSMALL_TRANSLATION_CACHE -DROM_BUFFER_SIZE=2 -DNSPIRE_LAYOUT_MIRROR
# LOOKUP_COUNT=1 (with BUILD=build/count, apart from the model's objects):
# perf counts the block lookups that reach C (cpu_threaded.c) and prints
# them a frame:
#   make -C tests/arm BUILD=build/count LOOKUP_COUNT=1 build/count/perf
#   tests/calc_speed.sh --scene ff --elf tests/arm/build/count/perf --tag count
CORE_DEFS  += $(if $(LOOKUP_COUNT),-DNSPIRE_LOOKUP_COUNT)
# GATE_TRAP=1 (with BUILD=build/gatetrap): an undefined instruction where the
# translator leaves out a block's translation gate (core-13, cpu_threaded.c);
# a replay that reaches one stops with SIGILL:
#   make -C tests/arm BUILD=build/gatetrap GATE_TRAP=1 CALC_ELF=- build/gatetrap/perf
CORE_DEFS  += $(if $(GATE_TRAP),-DNSPIRE_GATE_TRAP)
# IDLE_CHECK=1 (with BUILD=build/idlecheck): update_gba never runs a pure
# idle loop's next event itself (core-16, main.c); each time it would have,
# the loop's next real pass is checked to give the registers and flags the
# repeat assumed; perf prints the counts after the frames:
#   make -C tests/arm BUILD=build/idlecheck IDLE_CHECK=1 CALC_ELF=- build/idlecheck/perf
CORE_DEFS  += $(if $(IDLE_CHECK),-DNSPIRE_IDLE_CHECK)
# PCWORD_CHECK=1 (with BUILD=build/pcword): the translated code keeps the PC
# word after each memory access's call (as before core-17) and every lookup
# of nspire_mem_pc (arm_emit.h) is checked against it; perf prints the
# counts after the frames:
#   make -C tests/arm BUILD=build/pcword PCWORD_CHECK=1 CALC_ELF=- build/pcword/perf
CORE_DEFS  += $(if $(PCWORD_CHECK),-DNSPIRE_PCWORD_CHECK)
# KEEP_CHECK=1 (with BUILD=build/keepcheck): RAM blocks that would be
# copied from the kept ones (cpu_threaded.c "RAM blocks kept across Partial
# SMC's flushes") are translated instead and compared word for word with the
# copy, and each flush predicted instead of a copy checked to happen; perf
# prints the counts after the frames (stderr):
#   make -C tests/arm BUILD=build/keepcheck KEEP_CHECK=1 CALC_ELF=- build/keepcheck/perf
CORE_DEFS  += $(if $(KEEP_CHECK),-DNSPIRE_KEEP_CHECK)
# KEEP_TRACE=1: after each frame line (every=1), a "keep-trace:" line: a hash
# of every RAM block made (its tag, mode, PC, bytes and the scratch bit it
# read) and flush, and of the RAM cache's code, tag table, PC table and
# tags as the frame ends. run.sh compares such builds with the kept blocks
# and without (NOKEEP=1: as before speed10b) line by line.
CORE_DEFS  += $(if $(KEEP_TRACE),-DNSPIRE_KEEP_TRACE)
# RAMCACHE_KB=N (with BUILD=build/ramN): a RAM translation cache of N KB, so
# that its limits (a full cache's flush, the kept blocks' store reached by
# the code or the tag table) are met often.
CORE_DEFS  += $(if $(RAMCACHE_KB),-DNSPIRE_TEST_RAM_CACHE_KB=$(RAMCACHE_KB))
# ROMCACHE_KB=N: a ROM translation cache of N KB, flushed often (the kept
# RAM blocks' store emptied with it).
CORE_DEFS  += $(if $(ROMCACHE_KB),-DNSPIRE_TEST_ROM_CACHE_KB=$(ROMCACHE_KB))
# RAMPCS=1: perf's rampcs=N limits the RAM blocks' PC table to N entries
# (of 8192), its flushes often: copies of kept blocks decided next to them.
CORE_DEFS  += $(if $(RAMPCS),-DNSPIRE_TEST_RAM_PCS)
CORE_DEFS  += $(if $(NOKEEP),-DNSPIRE_NO_RAM_KEEP)
# GBA_PROFILE=1 (with BUILD=build/gbaprof): every GBA instruction the
# translated code runs is counted by its PC (arm_emit.h trace_instruction);
# perf prints the top a frame to stderr (proffrom=N proftop=N profdir=DIR):
#   make -C tests/arm BUILD=build/gbaprof GBA_PROFILE=1 CALC_ELF=- build/gbaprof/perf
CORE_DEFS  += $(if $(GBA_PROFILE),-DNSPIRE_GBA_PROFILE)
# LINE_CHECK=1 (with BUILD=build/linecheck CALC_ELF=-): speed10's lines kept
# as they were (nspire_reuse.c) are drawn anyway and compared with what was
# kept; perf prints the counts after the frames:
#   make -C tests/arm BUILD=build/linecheck LINE_CHECK=1 CALC_ELF=- build/linecheck/perf
CORE_DEFS  += $(if $(LINE_CHECK),-DNSPIRE_LINE_CHECK)
CORE_INCS  := -Ishim -I../host/include -I$(CORE) -I$(CORE)/libretro -I$(COMM)/include \
              -I$(ROOT) -I$(ROOT)/nspire -I$(ZLIB)
# The speed harness is linked with link-time optimization, as the calculator
# build is (cross-file inlining changes what the hot code costs); the objects
# keep plain code too (-ffat-lto-objects) for the other tests' links.
LTO := -flto -ffat-lto-objects
CORE_FLAGS := -O3 -msoft-float -funsigned-char -fno-common $(ARCH) -fomit-frame-pointer \
              -ffast-math -fno-reorder-functions -D_GNU_SOURCE $(CORE_DEFS) $(CORE_INCS) -w -MMD -MP \
              $(LTO)

CORE_C := main gba_memory savestate sound cheats memmap serial gbp rfu serial_proto serial_fe gba_cc_lut \
          cpu_threaded
CORE_C += serial_msio
COMM_C := compat/compat_posix_string compat/compat_strl compat/fopen_utf8 encodings/encoding_utf \
          file/file_path file/file_path_io streams/file_stream string/stdstring time/rtime \
          vfs/vfs_implementation
CORE_FRONT := nspire_stubs nspire_zip nspire_gbz nspire_render_time nspire_lines nspire_reuse
CORE_OBJS := $(addprefix $(BUILD)/core_,$(addsuffix .o,$(CORE_C))) $(BUILD)/core_cpu.o \
             $(addprefix $(BUILD)/comm_,$(addsuffix .o,$(notdir $(COMM_C)))) \
             $(addprefix $(BUILD)/front_,$(addsuffix .o,$(CORE_FRONT))) \
             $(BUILD)/bios_data.o $(BUILD)/arm_stub.o $(BUILD)/scanline_fill16.o \
             $(BUILD)/video_cc_renderer.o $(BUILD)/core_stubs.o \
             $(BUILD)/newlib_shim.o $(BUILD)/link_stubs.o

all: $(BUILD)/blend_test $(BUILD)/fill_test $(BUILD)/dynarec_test $(BUILD)/replay $(BUILD)/rtc_test $(BUILD)/ram_code.gba \
     $(BUILD)/badjump_test $(BUILD)/bad_jump_thumb.gba $(BUILD)/bad_jump_arm.gba $(BUILD)/dispatch_test \
     $(BUILD)/block_check_test $(BUILD)/idle_test $(BUILD)/idle_loop.gba $(BUILD)/perf \
     $(BUILD)/ui_bench $(BUILD)/libbench $(BUILD)/fastdeflate_test $(BUILD)/zehn_test \
     $(BUILD)/printf_test $(BUILD)/printf_test_ref

$(BUILD):
	mkdir -p $@

$(BUILD)/calc_%.o: $(ROOT)/%.c $(ROOT)/nspire_cold.mk | $(BUILD)
	nspire-gcc $(CALC_FLAGS) -c $< -o $@

# The frontend's cold files at -Os and the rest at -O2 but FRONT_O3_C, as
# the calculator build has them (../../nspire_cold.mk): ui_bench and
# libbench time that code. FRONT_O2_C: those of the frontend's files
# compiled here that are -O2 (nspire_fastdeflate.c is ui_bench's too).
# Their objects depend on nspire_cold.mk: a file moved between the lists is
# built again.
include $(ROOT)/nspire_cold.mk
$(addprefix $(BUILD)/calc_,$(addsuffix .o,$(COLD_C))): CALC_FLAGS := $(subst -O3,-Os,$(CALC_FLAGS))
$(addprefix $(BUILD)/calc_,$(addsuffix .o,$(FRONT_DATA_SECTIONS_C))): CALC_FLAGS += -fdata-sections

$(BUILD)/upscale_aspect.o: $(ROOT)/nspire/upscale_aspect.s | $(BUILD)
	arm-none-eabi-gcc $(ARCH) -c $< -o $@
$(BUILD)/blend_landscape.o: $(ROOT)/nspire/blend_landscape.S | $(BUILD)
	arm-none-eabi-gcc $(ARCH) -c $< -o $@
$(BUILD)/blend_portrait.o: $(ROOT)/nspire/blend_portrait.S | $(BUILD)
	arm-none-eabi-gcc $(ARCH) -c $< -o $@

$(BUILD)/test_%.o: %.c | $(BUILD)
	arm-none-eabi-gcc $(TEST_FLAGS) -c $< -o $@

# -fno-builtin: GCC would turn the shim's calloc into a call to calloc.
$(BUILD)/newlib_shim.o: newlib_shim.c | $(BUILD)
	arm-none-eabi-gcc $(ARCH) -O2 -fno-builtin -c $< -o $@

$(BUILD)/calc_blend_ref.o: blend_ref.c | $(BUILD)
	nspire-gcc $(CALC_FLAGS) -c $< -o $@

$(BUILD)/blend_test: $(BUILD)/test_blend_test.o $(BUILD)/calc_nspire_blend.o $(BUILD)/calc_nspire_turn.o \
                     $(BUILD)/calc_blend_ref.o $(BUILD)/upscale_aspect.o $(BUILD)/blend_landscape.o \
                     $(BUILD)/blend_portrait.o $(BUILD)/newlib_shim.o
	arm-none-eabi-gcc $(ARCH) -specs=rdimon.specs $^ -o $@

$(BUILD)/fill_test: $(BUILD)/test_fill_test.o $(BUILD)/scanline_fill16.o $(BUILD)/newlib_shim.o
	arm-none-eabi-gcc $(ARCH) -specs=rdimon.specs $^ -o $@

$(BUILD)/calc_ui_bench.o: ui_bench.c | $(BUILD)
	nspire-gcc $(CALC_FLAGS) -I$(ZLIB) -c $< -o $@

# (r16) Its constants and its buffers at fixed places: the model's caches are the calculator's, and a
# change of the code's size (or of a view's) moved the buffers' cache sets, which moved "the menu opens"
# (code nobody changed) by 0.3 ms. ui_bench.c pads the views to fixed sizes for the same reason.
UI_BENCH_PLACES := -Wl,--section-start=.rodata=0x40000,--section-start=.data=0x60000
$(BUILD)/ui_bench: $(BUILD)/calc_ui_bench.o $(BUILD)/calc_nspire_ui.o $(BUILD)/calc_nspire_ui_screens.o                    $(BUILD)/calc_nspire_turn.o $(BUILD)/calc_nspire_fastdeflate.o $(BUILD)/newlib_shim.o
	arm-none-eabi-gcc $(ARCH) -specs=rdimon.specs $^ $(LIBZ) $(UI_BENCH_PLACES) -o $@

$(BUILD)/calc_menu_bench.o: menu_bench.c | $(BUILD)
	nspire-gcc $(CALC_FLAGS) -I$(ZLIB) -c $< -o $@

$(BUILD)/menu_bench: $(BUILD)/calc_menu_bench.o $(BUILD)/calc_nspire_ui.o $(BUILD)/calc_nspire_ui_screens.o \
                     $(BUILD)/calc_nspire_turn.o $(BUILD)/newlib_shim.o
	arm-none-eabi-gcc $(ARCH) -specs=rdimon.specs $^ $(LIBZ) -o $@

$(BUILD)/calc_fastdeflate_test.o: fastdeflate_test.c | $(BUILD)
	nspire-gcc $(CALC_FLAGS) -I$(ZLIB) -c $< -o $@

$(BUILD)/test_zehn_test.o: zehn_test.c | $(BUILD)
	arm-none-eabi-gcc $(TEST_FLAGS) -I$(ZLIB) -c $< -o $@

$(BUILD)/zehn_test: $(BUILD)/test_zehn_test.o $(BUILD)/newlib_shim.o
	arm-none-eabi-gcc $(ARCH) -specs=rdimon.specs $^ $(LIBZ) -o $@

$(BUILD)/fastdeflate_test: $(BUILD)/calc_fastdeflate_test.o $(BUILD)/calc_nspire_fastdeflate.o $(BUILD)/newlib_shim.o
	arm-none-eabi-gcc $(ARCH) -specs=rdimon.specs $^ $(LIBZ) -o $@

# printf_test: newlib's printf family linked as the calculator links it
# (../../nspire_cold.mk PRINTF_LDFLAGS) and as newlib has it (_ref); run.sh
# compares their output. -fno-builtin: GCC would turn some calls into
# strcpy and the like.
$(BUILD)/printf_test.o: printf_test.c | $(BUILD)
	arm-none-eabi-gcc $(ARCH) -O2 -fno-builtin -w -c $< -o $@
$(BUILD)/printf_test: $(BUILD)/printf_test.o $(BUILD)/newlib_shim.o $(ROOT)/nspire_cold.mk
	arm-none-eabi-gcc $(ARCH) -specs=rdimon.specs $(PRINTF_LDFLAGS) $(filter %.o,$^) -o $@
$(BUILD)/printf_test_ref: $(BUILD)/printf_test.o $(BUILD)/newlib_shim.o
	arm-none-eabi-gcc $(ARCH) -specs=rdimon.specs $^ -o $@

# ---- The core ----

$(BUILD)/core_%.o: $(CORE)/%.c | $(BUILD)
	arm-none-eabi-gcc $(CORE_FLAGS) -std=gnu99 -c $< -o $@
$(BUILD)/core_cpu.o: $(CORE)/cpu.cc | $(BUILD)
	arm-none-eabi-g++ $(CORE_FLAGS) -std=gnu++11 -fno-exceptions -fno-rtti -c $< -o $@
define comm_rule
$(BUILD)/comm_$(notdir $(1)).o: $(COMM)/$(1).c | $(BUILD)
	arm-none-eabi-gcc $$(CORE_FLAGS) -std=gnu99 -c $$< -o $$@
endef
$(foreach f,$(COMM_C),$(eval $(call comm_rule,$(f))))
$(BUILD)/front_%.o: $(ROOT)/%.c | $(BUILD)
	arm-none-eabi-gcc $(CORE_FLAGS) -std=gnu99 -c $< -o $@
# speed10's nspire_reuse.c and nspire_pictures.c are the frontend's on the
# calculator (no LTO, -O2: the game's data stays where it was): the same here.
$(BUILD)/front_nspire_reuse.o: $(ROOT)/nspire_reuse.c | $(BUILD)
	arm-none-eabi-gcc $(subst -O3,-O2,$(filter-out -flto -ffat-lto-objects,$(CORE_FLAGS))) -ffunction-sections -std=gnu99 -c $< -o $@
$(BUILD)/front_nspire_pictures.o: $(ROOT)/nspire_pictures.c | $(BUILD)
	arm-none-eabi-gcc $(subst -O3,-O2,$(filter-out -flto -ffat-lto-objects,$(CORE_FLAGS))) -ffunction-sections -std=gnu99 -c $< -o $@
$(BUILD)/core_stubs.o: core_stubs.c | $(BUILD)
	arm-none-eabi-gcc $(CORE_FLAGS) -std=gnu99 -c $< -o $@
$(BUILD)/link_stubs.o: link_stubs.c | $(BUILD)
	arm-none-eabi-gcc $(CORE_FLAGS) -std=gnu99 -c $< -o $@
$(BUILD)/bios_data.o: $(CORE)/bios_data.S | $(BUILD)
	arm-none-eabi-gcc $(ARCH) -I$(CORE) -c $< -o $@
$(BUILD)/arm_stub.o: $(CORE)/arm/arm_stub.S | $(BUILD)
	arm-none-eabi-gcc $(ARCH) $(CORE_DEFS) $(CORE_INCS) -MMD -MP -c $< -o $@
$(BUILD)/scanline_fill16.o: $(ROOT)/nspire/scanline_fill16.S | $(BUILD)
	arm-none-eabi-gcc $(ARCH) -c $< -o $@
# video.cc's update_scanline renamed as in the calculator's Makefile.
$(BUILD)/video_cc_renderer.o: $(CORE)/video.cc | $(BUILD)
	arm-none-eabi-g++ $(CORE_FLAGS) $(VIDEO_OPT) -std=gnu++11 -fno-exceptions -fno-rtti \
	-Dupdate_scanline=video_cc_update_scanline -c $< -o $@

$(BUILD)/dynarec_test.o: dynarec_test.c | $(BUILD)
	arm-none-eabi-gcc $(CORE_FLAGS) -std=gnu99 -c $< -o $@
$(BUILD)/replay.o: replay.c | $(BUILD)
	arm-none-eabi-gcc $(CORE_FLAGS) -std=gnu99 -c $< -o $@
$(BUILD)/badjump_test.o: badjump_test.c | $(BUILD)
	arm-none-eabi-gcc $(CORE_FLAGS) -std=gnu99 -c $< -o $@
$(BUILD)/rtc_test.o: rtc_test.c | $(BUILD)
	arm-none-eabi-gcc $(CORE_FLAGS) -std=gnu99 -c $< -o $@
$(BUILD)/perf.o: perf.c | $(BUILD)
	arm-none-eabi-gcc $(CORE_FLAGS) $(PERF_RS_FLAGS) -std=gnu99 -c $< -o $@
$(BUILD)/perf_stubs.o: perf_stubs.c | $(BUILD)
	arm-none-eabi-gcc $(CORE_FLAGS) -std=gnu99 -c $< -o $@

# KEEP_TRACE builds: dynarec_test's data (reg, IWRAM, the caches) at a fixed
# address. The RAM cache's code holds those addresses and its hash is
# compared between the builds with the kept blocks and without, whose code
# differs in size: where the code ends must not move the data a page in one
# of them (r19-fx's renderer code did, at 7baa00e + 0.5 KB).
DYNAREC_TEST_LD := $(if $(KEEP_TRACE),-Xlinker -Tdata=0x200000)
$(BUILD)/dynarec_test: $(BUILD)/dynarec_test.o $(CORE_OBJS)
	arm-none-eabi-g++ $(ARCH) -specs=rdimon.specs $(DYNAREC_TEST_LD) $^ $(LIBZ) -lm -o $@
$(BUILD)/replay: $(BUILD)/replay.o $(CORE_OBJS)
	arm-none-eabi-g++ $(ARCH) -specs=rdimon.specs $^ $(LIBZ) -lm -o $@
$(BUILD)/badjump_test: $(BUILD)/badjump_test.o $(CORE_OBJS)
	arm-none-eabi-g++ $(ARCH) -specs=rdimon.specs $^ $(LIBZ) -lm -o $@
$(BUILD)/rtc_test: $(BUILD)/rtc_test.o $(CORE_OBJS)
	arm-none-eabi-g++ $(ARCH) -specs=rdimon.specs $^ $(LIBZ) -lm -o $@
$(BUILD)/dispatch_test.o: dispatch_test.c | $(BUILD)
	arm-none-eabi-gcc $(CORE_FLAGS) -std=gnu99 -c $< -o $@
$(BUILD)/dispatch_probe.o: dispatch_probe.S | $(BUILD)
	arm-none-eabi-gcc $(ARCH) -c $< -o $@
$(BUILD)/dispatch_test: $(BUILD)/dispatch_test.o $(BUILD)/dispatch_probe.o $(CORE_OBJS)
	arm-none-eabi-g++ $(ARCH) -specs=rdimon.specs $^ $(LIBZ) -lm -o $@
$(BUILD)/block_check_test.o: block_check_test.c | $(BUILD)
	arm-none-eabi-gcc $(CORE_FLAGS) -std=gnu99 -c $< -o $@
$(BUILD)/block_check_probe.o: block_check_probe.S | $(BUILD)
	arm-none-eabi-gcc $(ARCH) -c $< -o $@
$(BUILD)/block_check_test: $(BUILD)/block_check_test.o $(BUILD)/block_check_probe.o $(CORE_OBJS)
	arm-none-eabi-g++ $(ARCH) -specs=rdimon.specs $^ $(LIBZ) -lm -o $@
$(BUILD)/idle_test.o: idle_test.c | $(BUILD)
	arm-none-eabi-gcc $(CORE_FLAGS) -std=gnu99 -c $< -o $@
$(BUILD)/idle_test: $(BUILD)/idle_test.o $(CORE_OBJS)
	arm-none-eabi-g++ $(ARCH) -specs=rdimon.specs $^ $(LIBZ) -lm -o $@

# The speed harness: the core and the picture output (as the calculator
# shows a game frame), with perf_stubs.c's screen buffers.
PERF_FRONT := nspire_video_present nspire_video_ui nspire_osd nspire_ui nspire_blend nspire_turn nspire_upscale_from_gba \
              nspire_pictures
PERF_OBJS := $(filter-out $(BUILD)/core_stubs.o,$(CORE_OBJS)) $(BUILD)/perf_stubs.o \
             $(addprefix $(BUILD)/front_,$(addsuffix .o,$(PERF_FRONT))) $(BUILD)/upscale_aspect.o \
             $(BUILD)/blend_landscape.o $(BUILD)/blend_portrait.o
# The model runs the calculator's layout: linked once with every function in its own section,
# then again with mirror_layout.py's ordering file, which puts each where
# CALC_ELF (the calculator build) has it modulo 4 KB. Without CALC_ELF: the
# model's own layout, the hot code pinned as on the calculator.
# -fdata-sections (every variable its own section, put at the calculator's
# place too) costs the model ARM's section anchors: +0.7% game instructions
# against the calculator's code (Dewford 438,793 -> 442,163); a data move
# alone moved scenes by up to 6.6%, so the data's place weighs more.
# MIRROR_DATA=0: the calculator's exact code, LTO's data where it falls.
CALC_ELF ?= $(ROOT)/gpsp_libretro.elf
MIRROR_DATA ?= 1
PERF_LINK = arm-none-eabi-g++ $(ARCH) -O3 -flto -fno-reorder-functions -ffunction-sections \
            $(if $(filter 1,$(MIRROR_DATA)),-fdata-sections) -specs=rdimon.specs
PERF_IN = $(BUILD)/perf.o $(PERF_OBJS) $(BUILD)/layout_anchor.o
# AUDIO=1 with the USB sound driver beside the tree (as the top Makefile finds
# it): perf_audio's rs=RATE, the calculator's resampling into the driver's
# ring (tests/audio/perf_audio.h), its pcm.c built as the calculator's.
UA_DIR ?= $(if $(wildcard $(ROOT)/../nspire-usbaudio-gpsp/src/pcm.c),$(ROOT)/../nspire-usbaudio-gpsp,$(ROOT)/../nspire-usbaudio)
ifdef AUDIO
ifneq ($(wildcard $(UA_DIR)/src/pcm.c),)
PERF_RS_FLAGS := -DPERF_AUDIO_RS -I$(UA_DIR)/src -DRING_FRAMES=8192
PERF_IN += $(BUILD)/audio_pcm.o
$(BUILD)/perf.o: $(ROOT)/nspire_audio_mix.h ../audio/perf_audio.h
$(BUILD)/audio_pcm.o: $(UA_DIR)/src/pcm.c | $(BUILD)
	arm-none-eabi-gcc -O3 -std=c99 -msoft-float -funsigned-char -fno-common $(ARCH) -fomit-frame-pointer \
	  -DRING_FRAMES=8192 -I$(UA_DIR)/src -c $< -o $@
endif
endif
define perf_link   # output, calculator ELF or -, objects linked first
	$(PERF_LINK) $(3) $(PERF_IN) $(LIBZ) -lm -Wl,-Map=$(1).first.map -o $(1).first
	python3 -I mirror_layout.py write $(2) $(1).first.map $(1).first $(1).ld
	$(PERF_LINK) $(3) $(PERF_IN) $(LIBZ) -lm -Wl,--section-ordering-file=$(1).ld -o $(1)
	$(if $(filter-out -,$(2)),python3 -I mirror_layout.py check $(2) $(1) quiet)
	rm -f $(1).first $(1).first.map
endef
$(BUILD)/layout_anchor.o: layout_anchor.S | $(BUILD)
	arm-none-eabi-gcc $(ARCH) -c $< -o $@
$(BUILD)/perf: $(PERF_IN) mirror_layout.py $(wildcard $(CALC_ELF))
	$(call perf_link,$@,$(if $(wildcard $(CALC_ELF)),$(CALC_ELF),-))

# The model with another calculator build's layout (a layout move made in
# the calculator build: tests/layout_moves.sh), PAD (an object linked first,
# such as layout_pad.S's: the model's own data moves):
#   make build/perf-from FROM=path/to/gpsp_libretro.elf OUT=build/perf-x [PAD=obj]
FROM ?= -
OUT ?= $(BUILD)/perf-from
PAD ?=
$(BUILD)/perf-from: $(PERF_IN) mirror_layout.py FORCE
	$(call perf_link,$(OUT),$(FROM),$(PAD))
FORCE:

# AUDIO=1 (tests/audio): the same harness with the mixer on (no
# -DNSPIRE_NO_AUDIO), own objects in build/audio: make AUDIO=1 build/perf_audio
ifdef AUDIO
build/perf_audio: $(BUILD)/perf
	cp $< $@
endif

# The load/save benchmark: perf's core and picture output, the frontend's
# load and save files (with libbench_inc's dirent.h: the OS's folders), zlib
# (the SDK's sources built as the calculator build has them: ZLIB_C and
# ZLIB_FLAGS in ../../nspire_cold.mk) and newlib as on the calculator, under
# libbench_os.c (the SDK's libsyscalls and a stand-in for the OS) instead of
# librdimon's syscalls and newlib_shim.c's heap. The stdio calls are counted
# (--wrap).
LB_FRONT := nspire_states nspire_fastdeflate nspire_backup nspire_config nspire_paths nspire_selfstore nspire_rom_convert \
            nspire_game_list nspire_keys newlib_qsort newlib_strstr
LB_OBJS := $(filter-out $(BUILD)/core_stubs.o $(BUILD)/newlib_shim.o,$(CORE_OBJS)) \
           $(addprefix $(BUILD)/front_,$(addsuffix .o,$(PERF_FRONT))) $(BUILD)/upscale_aspect.o \
           $(BUILD)/blend_landscape.o $(BUILD)/blend_portrait.o \
           $(addprefix $(BUILD)/lb_,$(addsuffix .o,$(LB_FRONT))) $(BUILD)/libbench_os.o \
           $(if $(wildcard $(ZLIB)/crc32.c),$(addprefix $(BUILD)/zlib_,$(addsuffix .o,$(ZLIB_C))))
LB_WRAP := fopen fclose fread fwrite fseek ftell fseeko ftello fgets fputs fprintf fflush rename remove stat
$(BUILD)/lb_%.o: $(ROOT)/%.c $(ROOT)/nspire_cold.mk | $(BUILD)
	arm-none-eabi-gcc -Ilibbench_inc $(CORE_FLAGS) -std=gnu99 -c $< -o $@
$(addprefix $(BUILD)/lb_,$(addsuffix .o,$(COLD_C))): CORE_FLAGS := $(subst -O3,-Os,$(CORE_FLAGS))
FRONT_O2_C := $(filter-out $(COLD_C) $(FRONT_O3_C),$(LB_FRONT) nspire_fastdeflate)
$(addprefix $(BUILD)/lb_,$(addsuffix .o,$(FRONT_O2_C))): CORE_FLAGS := $(subst -O3,-O2,$(CORE_FLAGS))
$(addprefix $(BUILD)/calc_,$(addsuffix .o,$(FRONT_O2_C))): CALC_FLAGS := $(subst -O3,-O2,$(CALC_FLAGS))
$(BUILD)/zlib_%.o: $(ZLIB)/%.c $(ROOT)/nspire_cold.mk | $(BUILD)
	nspire-gcc $(ZLIB_FLAGS) -c $< -o $@
$(BUILD)/libbench.o: libbench.c | $(BUILD)
	arm-none-eabi-gcc -Ilibbench_inc $(CORE_FLAGS) -std=gnu99 -c $< -o $@
# No -flto, no builtins: the os_* functions stay functions (the model's
# region), and calloc stays malloc + memset.
$(BUILD)/libbench_os.o: libbench_os.c | $(BUILD)
	arm-none-eabi-gcc -Ilibbench_inc $(ARCH) -O2 -fno-builtin -Wall -MMD -MP -c $< -o $@
$(BUILD)/libbench: $(BUILD)/libbench.o $(LB_OBJS)
	arm-none-eabi-g++ $(ARCH) -O3 -flto -fno-reorder-functions -specs=rdimon.specs $^ $(LIBZ) -lm \
	$(addprefix -Wl$(comma)--wrap=,$(LB_WRAP)) -Wl,-Map=$@.map -o $@
comma := ,

# The test ROM: ARM code at the cartridge's address, as a plain image.
$(BUILD)/ram_code.gba: rom/ram_code.S | $(BUILD)
	arm-none-eabi-gcc $(ARCH) -nostdlib -Wl,-Ttext=0x08000000 -Wl,-e,_start $< -o $(BUILD)/ram_code.elf
	arm-none-eabi-objcopy -O binary $(BUILD)/ram_code.elf $@
$(BUILD)/idle_loop.gba: rom/idle_loop.S | $(BUILD)
	arm-none-eabi-gcc $(ARCH) -nostdlib -Wl,-Ttext=0x08000000 -Wl,-e,_start $< -o $(BUILD)/idle_loop.elf
	arm-none-eabi-objcopy -O binary $(BUILD)/idle_loop.elf $@
$(BUILD)/bad_jump_thumb.gba: rom/bad_jump.S | $(BUILD)
	arm-none-eabi-gcc $(ARCH) -nostdlib -Wl,-Ttext=0x08000000 -Wl,-e,_start -Wa,--defsym,THUMB=1 $< -o $(BUILD)/bad_jump_thumb.elf
	arm-none-eabi-objcopy -O binary $(BUILD)/bad_jump_thumb.elf $@
$(BUILD)/bad_jump_arm.gba: rom/bad_jump.S | $(BUILD)
	arm-none-eabi-gcc $(ARCH) -nostdlib -Wl,-Ttext=0x08000000 -Wl,-e,_start -Wa,--defsym,THUMB=0 $< -o $(BUILD)/bad_jump_arm.elf
	arm-none-eabi-objcopy -O binary $(BUILD)/bad_jump_arm.elf $@

clean:
	rm -rf $(BUILD)

# Header dependencies (-MMD): a changed arm_emit.h rebuilds cpu_threaded.
-include $(wildcard $(BUILD)/*.d)

.PHONY: all clean FORCE
