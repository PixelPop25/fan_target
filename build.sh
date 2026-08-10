#!/usr/bin/env bash
# One-shot build for fan_target_pxp
# Usage:  bash build.sh
# Optional:  PS5_PAYLOAD_SDK=/path/to/sdk bash build.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

echo "============================================"
echo " fan_target_pxp build"
echo "============================================"

# --- SDK ---
if [ -z "${PS5_PAYLOAD_SDK:-}" ]; then
  if [ -f sdk/toolchain/prospero.mk ]; then
    export PS5_PAYLOAD_SDK="$ROOT/sdk"
  elif [ -f /opt/ps5-payload-sdk/toolchain/prospero.mk ]; then
    export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
  else
    echo "PS5_PAYLOAD_SDK not found."
    echo "Download the binary SDK and either:"
    echo "  export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk"
    echo "or install it to /opt/ps5-payload-sdk"
    echo "  https://github.com/ps5-payload-dev/sdk/releases/latest"
    exit 1
  fi
fi
echo "Using SDK: $PS5_PAYLOAD_SDK"

# Host tools for the linker
if ! command -v ld.lld >/dev/null 2>&1 && [ ! -x /usr/lib/llvm-18/bin/ld.lld ]; then
  echo "Note: ld.lld not found — overlay/fps link may fail. Install lld (e.g. apt install lld-18)."
fi

# --- Submodules (etahen for fps_elf sources) ---
if [ -f .gitmodules ]; then
  echo "==> Ensuring git submodules..."
  git submodule update --init --recursive || true
fi

# --- Build payloads ---
echo "==> Building overlay_elf..."
make overlay_elf

echo "==> Building fps_elf from etahen submodule (with auto-patches)..."
if make fps_elf; then
  echo "fps_elf OK"
else
  echo "WARNING: fps_elf build failed — continuing with existing/empty blob"
fi

echo "==> Generating embed blobs..."
make blob

echo "==> Linking fan_target.elf..."
make all

echo ""
echo "============================================"
echo " Build complete"
echo "============================================"
if [ -f dist/fan_target.elf ]; then
  ls -lh dist/fan_target.elf
  cat dist/SHA256SUMS.txt 2>/dev/null || true
  echo ""
  echo "Load ONLY dist/fan_target.elf on the console."
  echo "Config will be created at /data/fan_target_pxp/config.ini on first run."
else
  echo "ERROR: dist/fan_target.elf was not produced"
  exit 1
fi
