# The rival gpSP (Vogtinator's gpSP 0.9 Nspire port, calc84maniac) under the
# ARM926 speed model: its core, renderer and picture output built as its
# Makefile does (-O3 -flto, soft float), with harness.c in place of its
# frontend (no LCD, keys from a script, frames counted). Its source is read
# from $(RIVAL) (not in this repository: GPL-2, the reference copy) and copied
# into build/src with the few calculator-only lines that can't run under
# qemu-arm's user mode taken out (the LCD's vblank flag, the CP15 code).
#   make -C tests/rival        (tests/calc_speed.sh --emu rival runs it)
RIVAL ?=
BUILD := build
SRC   := $(BUILD)/src
ARCH  := -marm -mcpu=arm926ej-s
FLAGS = -O3 -std=c99 -msoft-float -funsigned-char -DNSPIRE_BUILD -DARM_ARCH -flto $(ARCH) \
         -Ishim -I$(SRC) -I$(SRC)/nspire -I$(ZLIB) -w
LIBZ  := $(shell dirname $$(command -v nspire-gcc))/../lib/libz.a
ZLIB  := $(shell dirname $$(command -v nspire-gcc))/../thirdparty/zlib
CORE  := main cpu memory video sound cheats zip cpu_threaded
OBJS  := $(addprefix $(BUILD)/,$(addsuffix .o,$(CORE))) $(BUILD)/arm_stub.o $(BUILD)/video_blend.o \
         $(BUILD)/upscale_aspect.o $(BUILD)/harness.o $(BUILD)/newlib_shim.o

all: $(BUILD)/perf_rival

$(SRC)/.copied: $(wildcard $(RIVAL)/*.c $(RIVAL)/*.h $(RIVAL)/nspire/*)
	mkdir -p $(SRC)/nspire
	cp $(RIVAL)/*.c $(RIVAL)/*.h $(SRC)/
	cp $(RIVAL)/nspire/*.h $(RIVAL)/nspire/*.S $(RIVAL)/nspire/*.s $(SRC)/nspire/
	# main.c: the LCD's vblank flag (read every line) and the frame's pacing
	# come from harness.c; its entry point isn't used.
	sed -i -e 's/(\*(unsigned volatile\*)0xC0000020 & 4)/(rival_lcd_vblank())/' \
	       -e 's/\*(unsigned volatile\*)0xC0000028 = 4;//' \
	       -e 's/^void synchronize()/void rival_original_synchronize()/' \
	       -e 's/^int real_main(/int rival_real_main(/' $(SRC)/main.c
	# memory.c: the save written to a file when the game changes it (ours'
	# speed harness doesn't write either).
	sed -i -e 's/^u32 save_backup(char \*name)/u32 rival_original_save_backup(char *name)/' $(SRC)/memory.c
	# arm_stub.S: its crt0 replacement and the CP15 cache/alignment code.
	sed -i -e 's/^main:/rival_unused_main:/' -e 's/^\.global main$$//' \
	       -e 's/^clean_dcache:/rival_hw_clean_dcache:/' -e 's/^\.global clean_dcache$$//' \
	       -e 's/^enableAlignmentExceptions:/rival_hw_align:/' -e 's/^\.global enableAlignmentExceptions$$//' \
	       -e 's/b real_main/b rival_real_main/' \
	       -e 's/^not_crt0_savedsp: \.long 0/.pushsection .data\nnot_crt0_savedsp: .long 0\n.popsection/' \
	       $(SRC)/nspire/arm_stub.S
	# cpu_threaded.c: the translation caches in a section code can run from
	# (plain bss on the calculator, where all memory is executable; qemu-arm
	# honours the ELF's permissions).
	sed -i -e 's/^u8 \(rom\|ram\|bios\)_translation_cache\[/u8 __attribute__((section(".jit,\\"awx\\",%nobits@"))) \1_translation_cache[/' $(SRC)/cpu_threaded.c
	touch $@

$(BUILD)/%.o: $(SRC)/.copied
	arm-none-eabi-gcc $(FLAGS) -c $(SRC)/$*.c -o $@
$(BUILD)/arm_stub.o: $(SRC)/.copied
	arm-none-eabi-gcc $(FLAGS) -c $(SRC)/nspire/arm_stub.S -o $@
$(BUILD)/video_blend.o: $(SRC)/.copied
	arm-none-eabi-gcc $(FLAGS) -c $(SRC)/nspire/video_blend.S -o $@
$(BUILD)/upscale_aspect.o: $(SRC)/.copied
	arm-none-eabi-gcc $(ARCH) -c $(SRC)/nspire/upscale_aspect.s -o $@
$(BUILD)/harness.o: harness.c $(SRC)/.copied
	ln -sf $(RIVAL)/game_config.txt $(BUILD)/game_config.txt.tns
	arm-none-eabi-gcc $(FLAGS) -std=gnu99 -DRIVAL_CONFIG_DIR='"$(abspath $(BUILD))"' -c harness.c -o $@

$(BUILD)/newlib_shim.o: ../arm/newlib_shim.c
	arm-none-eabi-gcc -O2 $(ARCH) -c $< -o $@

# Linked at 0x10000000 as on the calculator (its SDRAM): its memory map keeps
# pointers less the GBA address and takes bit 31 for "a handler", so the ROM
# buffer must sit above 0x08000000 (from qemu's usual low addresses its ROM
# reads went to the handlers and it crashed).
$(BUILD)/perf_rival: $(OBJS)
	arm-none-eabi-gcc $(ARCH) -O3 -flto -specs=rdimon.specs -Wl,-Ttext-segment=0x10000000 $^ $(LIBZ) -lm -o $@

.PHONY: all
