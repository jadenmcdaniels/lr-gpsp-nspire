# R10b: gpSP's pad glue with the real nspire-usbpad (USBPAD_DIR/src) on the
# driver's fake TI USB stack (USBPAD_DIR/tests/os_fake), padsim_test.c.
# From tests/host: make -f ../padsim/padsim.mk [SAN=1] (tests/host/run.sh runs it).
include Makefile
SHIM_DIR ?= $(ROOT)/../nspire-usbshim
PADSIM_INC := -I$(USBPAD_DIR) -I$(USBPAD_DIR)/src -I$(USBPAD_DIR)/tests -I$(USBPAD_DIR)/tests/os_fake -I$(SHIM_DIR)
PADSIM_DRV := $(notdir $(basename $(wildcard $(USBPAD_DIR)/src/*.c)))
# The driver as its own native tests build it (USBPAD_HOST_TEST), its
# nspire_usbpad_active renamed as gpSP's Makefile does (usbpad_gpsp.o); the
# fake's nspire_link_cable_present renamed: gpSP's link has its own.
DRV_FLAGS := -O1 -g -std=gnu99 -w $(SANFLAGS) -DUSBPAD_HOST_TEST $(PADSIM_INC)
PADSIM_OBJS := $(addprefix $(BUILD)/padsim_drv_,$(addsuffix .o,$(PADSIM_DRV))) $(BUILD)/padsim_fake.o \
               $(BUILD)/padsim_glue.o $(BUILD)/padsim_test.o
.DEFAULT_GOAL := $(BUILD)/padsim_test

$(BUILD)/padsim_drv_%.o: $(USBPAD_DIR)/src/%.c $(wildcard $(USBPAD_DIR)/*.h $(USBPAD_DIR)/src/*.h) | $(BUILD)
	gcc $(DRV_FLAGS) $(if $(filter usbpad_core,$*),-Dnspire_usbpad_active=usbpad_lib_active) -c $< -o $@
$(BUILD)/padsim_fake.o: $(USBPAD_DIR)/tests/os_fake/fake_os.c $(USBPAD_DIR)/tests/os_fake/fake_os.h | $(BUILD)
	gcc $(DRV_FLAGS) -Dnspire_link_cable_present=fos_link_cable_present -c $< -o $@
$(BUILD)/padsim_glue.o: $(ROOT)/nspire_usbpad_glue.c | $(BUILD)
	gcc $(FRONT_FLAGS) $(PAD_FLAGS) -c $< -o $@
$(BUILD)/padsim_test.o: ../padsim/padsim_test.c | $(BUILD)
	gcc $(FRONT_FLAGS) $(PADSIM_INC) -DUSBPAD_HOST_TEST -c $< -o $@
$(BUILD)/padsim_test: $(UNIT_OBJS) $(PADSIM_OBJS)
	g++ $(SANFLAGS) $^ -lz -lpthread -o $@

# run.sh's target: built and run, or skipped without the driver's fake OS.
.PHONY: padsim-run
ifeq ($(wildcard $(USBPAD_DIR)/tests/os_fake/fake_os.c),)
padsim-run:
	@echo "padsim: skipped (no $(USBPAD_DIR)/tests/os_fake/fake_os.c)"
else
padsim-run: $(BUILD)/padsim_test
	./$(BUILD)/padsim_test $(BUILD)/padsim_work
endif
