# Independent builds of the existing state/tone references and runtime API.
# Separate BUILD paths prevent flags from reusing incompatible core objects.
include ../audio/tone.mk

ifeq ($(RUNTIME),1)
CORE_DEFS := $(filter-out -DNSPIRE_NO_AUDIO,$(CORE_DEFS)) -DNSPIRE_RUNTIME_AUDIO
CORE_FLAGS := $(filter-out -DNSPIRE_NO_AUDIO,$(CORE_FLAGS)) -DNSPIRE_RUNTIME_AUDIO
endif

$(BUILD)/runtime_state_test.o: ../audio/runtime_state_test.c ../audio/state_test.c | $(BUILD)
	arm-none-eabi-gcc $(CORE_FLAGS) -std=gnu99 -c $< -o $@
$(BUILD)/runtime_state_test: $(BUILD)/runtime_state_test.o $(CORE_OBJS)
	arm-none-eabi-g++ $(ARCH) -specs=rdimon.specs $^ $(LIBZ) -lm -o $@

$(BUILD)/runtime_toggle_test.o: ../audio/runtime_toggle_test.c | $(BUILD)
	arm-none-eabi-gcc $(CORE_FLAGS) -std=gnu99 -c $< -o $@
$(BUILD)/runtime_toggle_test: $(BUILD)/runtime_toggle_test.o $(CORE_OBJS)
	arm-none-eabi-g++ $(ARCH) -specs=rdimon.specs $^ $(LIBZ) -lm -o $@
