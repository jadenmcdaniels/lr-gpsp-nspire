# Real gpSP mixer + real audio adapter, with the TI USB task simulated.
# From tests/host: make -f ../audio/sink.mk BUILD=build-sink SAN=1
include Makefile
DEFS := $(filter-out -DNSPIRE_NO_AUDIO,$(DEFS)) -DNSPIRE_RUNTIME_AUDIO
CORE_FLAGS := $(filter-out -DNSPIRE_NO_AUDIO,$(CORE_FLAGS)) -DNSPIRE_RUNTIME_AUDIO
FRONT_FLAGS := $(filter-out -DNSPIRE_NO_AUDIO,$(FRONT_FLAGS)) -DNSPIRE_RUNTIME_AUDIO
# The driver gpSP builds: the worktree kept for gpSP when it is there (as
# the top Makefile; the other is a separate work tree).
USB_AUDIO_DIR ?= $(if $(wildcard $(ROOT)/../nspire-usbaudio-gpsp/src/ua_os.h),$(ROOT)/../nspire-usbaudio-gpsp,$(ROOT)/../nspire-usbaudio)
SHIM_DIR ?= $(ROOT)/../nspire-usbshim
# Transfers queued and their packets, as the top Makefile (GPSP_UA_NX, GPSP_UA_NF).
GPSP_UA_NX ?= 4
GPSP_UA_NF ?= 14
GPSP_UA_START ?= -DUA_START_ONE
AUDIO_FLAGS := -I$(USB_AUDIO_DIR)/src -I$(USB_AUDIO_DIR)/tests -I$(SHIM_DIR) -DUA_HOST_TEST -DRING_FRAMES=16384 \
               -DUA_NX=$(GPSP_UA_NX) -DUA_NF=$(GPSP_UA_NF) $(GPSP_UA_START)
.DEFAULT_GOAL := $(BUILD)/sink_test
# The PlayStation pads' real configurations (the gamepad driver's corpus).
SINK_PAD_INC := $(if $(wildcard $(USBPAD_DIR)/tests/corpus/cfg_sony_real.h),-I$(USBPAD_DIR)/tests -DSINK_HAVE_SONY)
$(BUILD)/sink_test.o: ../audio/sink_test.c | $(BUILD)
	gcc $(FRONT_FLAGS) $(AUDIO_FLAGS) $(SINK_PAD_INC) -c $< -o $@
$(BUILD)/sink_front.o: $(ROOT)/nspire_audio.c | $(BUILD)
	gcc $(FRONT_FLAGS) $(AUDIO_FLAGS) -c $< -o $@
# ua_os.c as gpSP builds it: through nspire_audio_os.c (2026-11-15).
$(BUILD)/sink_ua_os.o: $(ROOT)/nspire_audio_os.c $(USB_AUDIO_DIR)/src/ua_os.c | $(BUILD)
	gcc $(FRONT_FLAGS) $(AUDIO_FLAGS) -c $< -o $@
$(BUILD)/sink_%.o: $(USB_AUDIO_DIR)/src/%.c | $(BUILD)
	gcc $(FRONT_FLAGS) $(AUDIO_FLAGS) -c $< -o $@
$(BUILD)/sink_fake.o: $(USB_AUDIO_DIR)/tests/fake/fake_os.c | $(BUILD)
	gcc $(FRONT_FLAGS) $(AUDIO_FLAGS) -c $< -o $@
$(BUILD)/sink_test: $(UNIT_OBJS) $(BUILD)/sink_test.o $(BUILD)/sink_front.o $(BUILD)/sink_pcm.o $(BUILD)/sink_uac.o $(BUILD)/sink_ua_os.o $(BUILD)/sink_fake.o
	g++ $(SANFLAGS) $^ -Wl,--wrap=nspire_os_sleep -Wl,--wrap=nspire_bt_glue_audio_frame \
	    -Wl,--wrap=nspire_bt_glue_audio_write_mix -Wl,--wrap=nspire_bt_glue_audio_delay_ms -lz -o $@
