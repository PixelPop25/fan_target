ifneq ($(wildcard sdk/toolchain/prospero.mk),)
  PS5_PAYLOAD_SDK ?= $(CURDIR)/sdk
else ifneq ($(wildcard $(CURDIR)/sdk-install/toolchain/prospero.mk),)
  PS5_PAYLOAD_SDK ?= $(CURDIR)/sdk-install
else
  PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
endif

ifeq ($(wildcard $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk),)
$(error PS5_PAYLOAD_SDK does not point to a valid SDK: $(PS5_PAYLOAD_SDK))
endif

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

DIST_DIR ?= dist
GEN_DIR := gen
SRC := main.c $(GEN_DIR)/fps_elf_blob.c
ELF := $(DIST_DIR)/fan_target.elf

FPS_ELF_SRC_DIR := third_party/fps_elf
FPS_ELF_BUILD_DIR := $(FPS_ELF_SRC_DIR)/build
FPS_ELF_BIN := $(FPS_ELF_SRC_DIR)/bin/fps_elf.elf

CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2
CPPFLAGS := -I$(GEN_DIR)
LDLIBS := -lkernel_sys -lScePad -lSceUserService
ELF_STRIP := $(firstword $(wildcard $(PS5_PAYLOAD_SDK)/bin/prospero-llvm-strip) \
	$(wildcard $(PS5_PAYLOAD_SDK)/bin/prospero-strip))

.PHONY: all fps_elf blob sums clean clean-fps_elf

# `make all` builds fan_target.elf. If third_party/fps_elf/bin/fps_elf.elf
# doesn't exist yet (i.e. you haven't run `make fps_elf`), the fps overlay
# is embedded as an empty placeholder and main.c logs "not embedded" and
# skips it at runtime instead of failing to build. Run `make fps_elf all`
# to get a build with the overlay actually included.
all: $(ELF) sums

$(DIST_DIR) $(GEN_DIR):
	mkdir -p $@

# Builds the third-party fps overlay ELF with its own CMake project. This
# needs a PS5 SDK layout with SceGnmDriver (i.e. a Sony-flavoured PS5
# payload SDK, not just the base ps5-payload-dev/sdk toolchain) -- if you
# don't have that, skip this target; `make all` on its own still produces
# a working fan_target.elf with the overlay disabled.
fps_elf:
	cmake -S $(FPS_ELF_SRC_DIR) -B $(FPS_ELF_BUILD_DIR) \
		-DPS5_PAYLOAD_SDK=$(PS5_PAYLOAD_SDK) \
		-DCMAKE_C_COMPILER=$(PS5_PAYLOAD_SDK)/bin/prospero-clang \
		-DCMAKE_CXX_COMPILER=$(PS5_PAYLOAD_SDK)/bin/prospero-clang++
	cmake --build $(FPS_ELF_BUILD_DIR)

# Regenerates gen/fps_elf_blob.{c,h} from whatever fps_elf.elf currently
# exists. Safe to run even if fps_elf hasn't been built -- see
# tools/gen_fps_elf_blob.py for the placeholder behaviour.
blob: | $(GEN_DIR)
	python3 tools/gen_fps_elf_blob.py $(FPS_ELF_BIN) $(GEN_DIR)

$(GEN_DIR)/fps_elf_blob.c $(GEN_DIR)/fps_elf_blob.h: blob
	@:

$(ELF): $(SRC) | $(DIST_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ main.c $(GEN_DIR)/fps_elf_blob.c $(LDLIBS)
	@$(if $(ELF_STRIP),$(ELF_STRIP) --strip-all $@,:)

sums: $(ELF)
	cd $(DIST_DIR) && sha256sum $(notdir $(ELF)) > SHA256SUMS.txt

clean:
	rm -rf $(DIST_DIR) $(GEN_DIR)

clean-fps_elf:
	rm -rf $(FPS_ELF_BUILD_DIR) $(FPS_ELF_SRC_DIR)/bin
