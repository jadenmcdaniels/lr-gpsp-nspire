# Independent discontinuity ordering checks using the existing sink fixture.
# From tests/host: make -f ../audio/sink_boundaries.mk BUILD=build-sink-bounds SAN=1
include ../audio/sink.mk
.DEFAULT_GOAL := $(BUILD)/sink_boundaries_test

$(BUILD)/sink_boundaries_test.o: ../audio/sink_boundaries_test.c ../audio/sink_test.c | $(BUILD)
	gcc $(FRONT_FLAGS) $(AUDIO_FLAGS) $(SINK_PAD_INC) -c $< -o $@
$(BUILD)/sink_boundaries_test: $(UNIT_OBJS) $(BUILD)/sink_boundaries_test.o $(BUILD)/sink_front.o $(BUILD)/sink_pcm.o $(BUILD)/sink_uac.o $(BUILD)/sink_ua_os.o $(BUILD)/sink_fake.o
	g++ $(SANFLAGS) $^ -Wl,--wrap=nspire_os_sleep -Wl,--wrap=ua_set_pause -Wl,--wrap=nspire_bt_glue_audio_frame \
	    -Wl,--wrap=nspire_bt_glue_audio_write_mix -Wl,--wrap=nspire_bt_glue_audio_delay_ms -lz -o $@
