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
SRC := main.c $(GEN_DIR)/fps_elf_blob.c $(GEN_DIR)/overlay_elf_blob.c
ELF := $(DIST_DIR)/fan_target.elf

FPS_ELF_SRC_DIR := third_party/fps_elf
FPS_ELF_BUILD_DIR := $(FPS_ELF_SRC_DIR)/build
FPS_ELF_BIN := $(FPS_ELF_SRC_DIR)/bin/fps_elf.elf
OVERLAY_ELF_BIN := third_party/overlay_elf/bin/overlay_elf.elf

CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2
CPPFLAGS := -I$(GEN_DIR)
LDLIBS := -lkernel_sys -lScePad -lSceUserService
ELF_STRIP := $(firstword $(wildcard $(PS5_PAYLOAD_SDK)/bin/prospero-llvm-strip) \
	$(wildcard $(PS5_PAYLOAD_SDK)/bin/prospero-strip))

.PHONY: all fps_elf overlay_elf blob sums clean clean-fps_elf

# make all → fan_target.elf
# Embed real payloads after: make fps_elf overlay_elf blob all
all: $(ELF) sums

$(DIST_DIR) $(GEN_DIR):
	mkdir -p $@

fps_elf:
	cmake -S $(FPS_ELF_SRC_DIR) -B $(FPS_ELF_BUILD_DIR) \
		-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
		-DPS5_PAYLOAD_SDK=$(PS5_PAYLOAD_SDK) \
		-DCMAKE_C_COMPILER=$(PS5_PAYLOAD_SDK)/bin/prospero-clang \
		-DCMAKE_CXX_COMPILER=$(PS5_PAYLOAD_SDK)/bin/prospero-clang++
	cmake --build $(FPS_ELF_BUILD_DIR)

# Overlay is a single C++ translation unit — build with prospero-clang++ if available
overlay_elf:
	mkdir -p third_party/overlay_elf/bin
	$(PS5_PAYLOAD_SDK)/bin/prospero-clang++ -std=c++17 -O2 \
		-I$(PS5_PAYLOAD_SDK)/target/include \
		-Ithird_party/overlay_elf/include \
		-L$(PS5_PAYLOAD_SDK)/target/lib \
		-o $(OVERLAY_ELF_BIN) third_party/overlay_elf/src/prx.cpp \
		-lkernel_sys -lc++ -lc++abi -lunwind || true

blob: | $(GEN_DIR)
	python3 tools/gen_fps_elf_blob.py $(FPS_ELF_BIN) $(GEN_DIR) fps_elf
	python3 tools/gen_fps_elf_blob.py $(OVERLAY_ELF_BIN) $(GEN_DIR) overlay_elf

$(GEN_DIR)/fps_elf_blob.c $(GEN_DIR)/fps_elf_blob.h \
$(GEN_DIR)/overlay_elf_blob.c $(GEN_DIR)/overlay_elf_blob.h: blob
	@:

$(ELF): main.c $(GEN_DIR)/fps_elf_blob.c $(GEN_DIR)/overlay_elf_blob.c | $(DIST_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ main.c \
		$(GEN_DIR)/fps_elf_blob.c $(GEN_DIR)/overlay_elf_blob.c $(LDLIBS)
	@$(if $(ELF_STRIP),$(ELF_STRIP) --strip-all $@,:)

sums: $(ELF)
	cd $(DIST_DIR) && sha256sum $(notdir $(ELF)) > SHA256SUMS.txt

clean:
	rm -rf $(DIST_DIR)

clean-fps_elf:
	rm -rf $(FPS_ELF_BUILD_DIR) $(FPS_ELF_SRC_DIR)/bin third_party/overlay_elf/bin
