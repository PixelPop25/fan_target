# Prefer a local sdk/ checkout (git submodule) over the system install.
#   git submodule add https://github.com/ps5-payload-dev/sdk.git sdk
#   # then either build/install the SDK once, or point at a prebuilt tree:
#   export PS5_PAYLOAD_SDK=$(pwd)/sdk-install   # or /opt/ps5-payload-sdk
#
# The SDK repo is the *source*; you still need a built/install tree that
# contains toolchain/prospero.mk. Easiest path for most people:
#   wget the release zip → unzip to /opt/ps5-payload-sdk
# Or use the submodule + its own make install into a local prefix.

ifneq ($(wildcard sdk/toolchain/prospero.mk),)
  PS5_PAYLOAD_SDK ?= $(CURDIR)/sdk
else ifneq ($(wildcard $(CURDIR)/sdk-install/toolchain/prospero.mk),)
  PS5_PAYLOAD_SDK ?= $(CURDIR)/sdk-install
else
  PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
endif

ifeq ($(wildcard $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk),)
$(error PS5_PAYLOAD_SDK does not point to a valid SDK: $(PS5_PAYLOAD_SDK)
Hint: install a release under /opt/ps5-payload-sdk, or add the submodule:
  git submodule add https://github.com/ps5-payload-dev/sdk.git sdk
  # then build/install it, e.g.:
  #   make -C sdk DESTDIR=$(CURDIR)/sdk-install install
  # and re-run make)
endif

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

DIST_DIR ?= dist
SRC := main.c
ELF := $(DIST_DIR)/fan_target.elf

CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2
LDLIBS := -lkernel_sys
ELF_STRIP := $(firstword $(wildcard $(PS5_PAYLOAD_SDK)/bin/prospero-llvm-strip) \
	$(wildcard $(PS5_PAYLOAD_SDK)/bin/prospero-strip))

.PHONY: all sums clean

all: $(ELF) sums

$(DIST_DIR):
	mkdir -p $@

$(ELF): $(SRC) | $(DIST_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)
	@$(if $(ELF_STRIP),$(ELF_STRIP) --strip-all $@,:)

sums: $(ELF)
	cd $(DIST_DIR) && sha256sum $(notdir $(ELF)) > SHA256SUMS.txt

clean:
	rm -rf $(DIST_DIR)
