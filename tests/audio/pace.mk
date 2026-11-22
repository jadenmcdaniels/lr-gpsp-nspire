# The pacing model (pace_test.c): the real nspire_audio.c, nspire_frameskip.c,
# mixer and audio driver in virtual time on the driver's fake TI USB stack.
# From tests/host: make -f ../audio/pace.mk BUILD=build-sink SAN=1
# PACE_SRC=DIR: nspire_audio.c and nspire_frameskip.c from DIR instead (an
# older tree's, e.g. faf629e's with GPSP_UA_NX=6 and a BUILD of its own:
# tests/audio/pace_old.sh).
include ../audio/sink.mk
PACE_SRC ?= $(ROOT)
.DEFAULT_GOAL := $(BUILD)/pace_test
$(BUILD)/pace_test.o: ../audio/pace_test.c | $(BUILD)
	gcc $(FRONT_FLAGS) $(AUDIO_FLAGS) -c $< -o $@
$(BUILD)/pace_audio.o: $(PACE_SRC)/nspire_audio.c | $(BUILD)
	gcc $(FRONT_FLAGS) $(AUDIO_FLAGS) -c $< -o $@
# (host_stubs.c has its own nspire_fps_overlay_reset, for the core's reset_gba)
$(BUILD)/pace_frameskip.o: $(PACE_SRC)/nspire_frameskip.c | $(BUILD)
	gcc $(FRONT_FLAGS) -Dnspire_fps_overlay_reset=pace_fps_overlay_reset -c $< -o $@
$(BUILD)/pace_test: $(UNIT_OBJS) $(BUILD)/pace_test.o $(BUILD)/pace_audio.o $(BUILD)/pace_frameskip.o \
                    $(BUILD)/sink_pcm.o $(BUILD)/sink_uac.o $(BUILD)/sink_ua_os.o $(BUILD)/sink_fake.o
	g++ $(SANFLAGS) $^ -Wl,--wrap=nspire_os_sleep -Wl,--wrap=nspire_os_sleeps -Wl,--wrap=nspire_os_busy_slept \
	  -Wl,--wrap=nspire_rtc_seconds -Wl,--wrap=nspire_bt_glue_audio_frame -Wl,--wrap=nspire_bt_glue_audio_write_mix -Wl,--wrap=nspire_bt_glue_audio_delay_ms \
	  -lz -lm -o $@
