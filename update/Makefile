# fan_target_pxp Makefile
# Requires: PS5 Payload SDK (binary release) at $PS5_PAYLOAD_SDK or /opt/ps5-payload-sdk

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
ELF := $(DIST_DIR)/fan_target.elf

FPS_ELF_SRC_DIR := third_party/fps_elf
FPS_ELF_BUILD_DIR := $(FPS_ELF_SRC_DIR)/build
FPS_ELF_BIN := $(FPS_ELF_SRC_DIR)/bin/fps_elf.elf
OVERLAY_ELF_BIN := third_party/overlay_elf/bin/overlay_elf.elf

# Preferred: rebuild fps_elf from the etahen submodule sources
ETAHEN_DIR := third_party/etahen
ETAHEN_FPS := $(ETAHEN_DIR)/Source Code/fps_elf
ETAHEN_STAGING := third_party/.etahen_fps_build

CFLAGS ?= -std=c11 -Wall -Wextra -O2 -Wno-unused-function
CPPFLAGS := -I$(GEN_DIR)
LDLIBS := -lkernel_sys -lScePad -lSceUserService -lpthread
ELF_STRIP := $(firstword $(wildcard $(PS5_PAYLOAD_SDK)/bin/prospero-llvm-strip) \
	$(wildcard $(PS5_PAYLOAD_SDK)/bin/prospero-strip) \
	$(wildcard $(PS5_PAYLOAD_SDK)/bin/llvm-strip))

.PHONY: all fps_elf fps_elf_from_etahen overlay_elf blob sums clean clean-fps_elf submodules

# Default: embed whatever ELFs are present (or empty stubs) and link fan_target.elf
all: $(ELF) sums

$(DIST_DIR) $(GEN_DIR):
	mkdir -p $@

# ---------------------------------------------------------------
# fps_elf — prefer building from the etahen submodule
# ---------------------------------------------------------------
fps_elf: fps_elf_from_etahen

fps_elf_from_etahen:
	@if [ ! -d "$(ETAHEN_FPS)" ]; then \
	  echo "==> etahen submodule missing — running git submodule update..."; \
	  git submodule update --init --recursive third_party/etahen || \
	    (echo "ERROR: could not init third_party/etahen. Run: git submodule update --init --recursive" && exit 1); \
	fi
	@echo "==> Staging etaHEN fps_elf sources + applying standalone patches..."
	@rm -rf "$(ETAHEN_STAGING)"
	@mkdir -p "$(ETAHEN_STAGING)"
	@cp -a "$(ETAHEN_DIR)/Source Code/fps_elf" "$(ETAHEN_STAGING)/"
	@cp -a "$(ETAHEN_DIR)/Source Code/include" "$(ETAHEN_STAGING)/"
	@cp -a "$(ETAHEN_DIR)/Source Code/lib" "$(ETAHEN_STAGING)/"
	@cp -a "$(ETAHEN_DIR)/Source Code/extern" "$(ETAHEN_STAGING)/" 2>/dev/null || true
	@cp -a "$(ETAHEN_DIR)/Source Code/linker.x" "$(ETAHEN_STAGING)/" 2>/dev/null || true
	@python3 tools/patch_etahen_fps_cmake.py "$(ETAHEN_STAGING)/fps_elf/CMakeLists.txt"
	@# Ensure matching headers land in the local include tree
	@cp -f "$(ETAHEN_STAGING)/include/msg.hpp" "$(ETAHEN_STAGING)/fps_elf/include/" 2>/dev/null || true
	@cp -f "$(ETAHEN_STAGING)/include/json.hpp" "$(ETAHEN_STAGING)/fps_elf/include/" 2>/dev/null || true
	@# Soften enum underlying type for large constants
	@sed -i 's/enum DaemonCommands *{/enum DaemonCommands : unsigned int {/' "$(ETAHEN_STAGING)/include/msg.hpp" 2>/dev/null || true
	@sed -i 's/enum DaemonCommands : unsigned int cmd;/DaemonCommands cmd;/' "$(ETAHEN_STAGING)/include/msg.hpp" 2>/dev/null || true
	@sed -i 's/enum DaemonCommands : unsigned int cmd;/DaemonCommands cmd;/' "$(ETAHEN_STAGING)/fps_elf/include/msg.hpp" 2>/dev/null || true
	@mkdir -p "$(ETAHEN_STAGING)/fps_elf/bin" "$(ETAHEN_STAGING)/fps_elf/build"
	@echo "==> Configuring fps_elf..."
	cmake -S "$(ETAHEN_STAGING)/fps_elf" -B "$(ETAHEN_STAGING)/fps_elf/build" \
		-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
		-DPS5_PAYLOAD_SDK=$(PS5_PAYLOAD_SDK) \
		-DCMAKE_C_COMPILER=$(PS5_PAYLOAD_SDK)/bin/prospero-clang \
		-DCMAKE_CXX_COMPILER=$(PS5_PAYLOAD_SDK)/bin/prospero-clang++ \
		-DCMAKE_OBJCOPY=$(PS5_PAYLOAD_SDK)/bin/llvm-objcopy
	@echo "==> Building fps_elf..."
	cmake --build "$(ETAHEN_STAGING)/fps_elf/build" || true
	@# Accept either the linked ELF or a partially-stripped one
	@ELF_OUT=""; \
	for c in "$(ETAHEN_STAGING)/fps_elf/bin/fps_elf.elf" \
	         "$(ETAHEN_STAGING)/fps_elf/bin/fps_elf.stripped.elf"; do \
	  if [ -f "$$c" ] && [ -s "$$c" ]; then ELF_OUT="$$c"; break; fi; \
	done; \
	if [ -z "$$ELF_OUT" ]; then \
	  echo "ERROR: fps_elf.elf was not produced"; exit 1; \
	fi; \
	mkdir -p "$(FPS_ELF_SRC_DIR)/bin"; \
	cp -f "$$ELF_OUT" "$(FPS_ELF_BIN)"; \
	echo "==> Installed $$(ls -lh $(FPS_ELF_BIN) | awk '{print $$5}') → $(FPS_ELF_BIN)"

# Fallback: build from the vendored third_party/fps_elf tree (if present)
fps_elf_local:
	cmake -S $(FPS_ELF_SRC_DIR) -B $(FPS_ELF_BUILD_DIR) \
		-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
		-DPS5_PAYLOAD_SDK=$(PS5_PAYLOAD_SDK) \
		-DCMAKE_C_COMPILER=$(PS5_PAYLOAD_SDK)/bin/prospero-clang \
		-DCMAKE_CXX_COMPILER=$(PS5_PAYLOAD_SDK)/bin/prospero-clang++
	cmake --build $(FPS_ELF_BUILD_DIR)

overlay_elf:
	mkdir -p third_party/overlay_elf/bin
	$(PS5_PAYLOAD_SDK)/bin/prospero-clang++ -std=c++17 -O2 \
		-I$(PS5_PAYLOAD_SDK)/target/include \
		-Ithird_party/overlay_elf/include \
		-o $(OVERLAY_ELF_BIN) third_party/overlay_elf/src/prx.cpp \
		-lkernel_sys

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

submodules:
	git submodule update --init --recursive

clean:
	rm -rf $(DIST_DIR) $(GEN_DIR) $(ETAHEN_STAGING)

clean-fps_elf:
	rm -rf $(FPS_ELF_BUILD_DIR) $(FPS_ELF_SRC_DIR)/bin third_party/overlay_elf/bin $(ETAHEN_STAGING)
