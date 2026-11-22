# nspire_audio_mix.h against the three passes it replaces (mix_test.c), with
# the driver's pcm.c. From tests/host: make -f ../audio/mix.mk BUILD=build-sink SAN=1
include ../audio/sink.mk
.DEFAULT_GOAL := $(BUILD)/mix_test
$(BUILD)/mix_test.o: ../audio/mix_test.c $(ROOT)/nspire_audio_mix.h | $(BUILD)
	gcc $(FRONT_FLAGS) $(AUDIO_FLAGS) -c $< -o $@
$(BUILD)/mix_test: $(BUILD)/mix_test.o $(BUILD)/sink_pcm.o
	gcc $(SANFLAGS) $^ -o $@
