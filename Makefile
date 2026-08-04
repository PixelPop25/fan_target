PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk

ifeq ($(wildcard $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk),)
$(error PS5_PAYLOAD_SDK does not point to a valid SDK: $(PS5_PAYLOAD_SDK))
endif

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

TARGETS ?= 85 80 75 70 65
DIST_DIR ?= dist
SRC := main.c
ELFS := $(addprefix $(DIST_DIR)/fan_target_,$(addsuffix c.elf,$(TARGETS)))

CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2
LDLIBS := -lkernel_sys
ELF_STRIP := $(firstword $(wildcard $(PS5_PAYLOAD_SDK)/bin/prospero-llvm-strip) \
	$(wildcard $(PS5_PAYLOAD_SDK)/bin/prospero-strip))

.PHONY: all variants sums clean

all: variants sums

variants: $(ELFS)

$(DIST_DIR):
	mkdir -p $@

$(DIST_DIR)/fan_target_%c.elf: $(SRC) | $(DIST_DIR)
	$(CC) $(CFLAGS) -DFANTARGET_TARGET_C=$* -o $@ $< $(LDLIBS)
	@$(if $(ELF_STRIP),$(ELF_STRIP) --strip-all $@,:)

sums: variants
	cd $(DIST_DIR) && sha256sum $(notdir $(ELFS)) > SHA256SUMS.txt

clean:
	rm -rf $(DIST_DIR)
