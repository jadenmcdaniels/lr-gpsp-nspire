# The mixer's synthetic tone test (tests/audio/tone_test.c): the core as the
# audio harness builds it (AUDIO=1, tests/arm/Makefile), linked with the test.
#   make -C tests/arm -f ../audio/tone.mk AUDIO=1 build/audio/tone_test
include Makefile

$(BUILD)/tone_test.o: ../audio/tone_test.c | $(BUILD)
	arm-none-eabi-gcc $(CORE_FLAGS) -std=gnu99 -c $< -o $@
$(BUILD)/tone_test: $(BUILD)/tone_test.o $(CORE_OBJS)
	arm-none-eabi-g++ $(ARCH) -specs=rdimon.specs $^ $(LIBZ) -lm -o $@

$(BUILD)/sound_state_test.o: ../audio/state_test.c | $(BUILD)
	arm-none-eabi-gcc $(CORE_FLAGS) -std=gnu99 -c $< -o $@
$(BUILD)/sound_state_test: $(BUILD)/sound_state_test.o $(CORE_OBJS)
	arm-none-eabi-g++ $(ARCH) -specs=rdimon.specs $^ $(LIBZ) -lm -o $@
