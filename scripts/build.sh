#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
SRC_DIR="$PROJECT_DIR/src"
OUT_DIR="$PROJECT_DIR/dist/move-everything-structor"

# Cross-compiler prefix (Docker or local toolchain)
CC="${CROSS_PREFIX:-aarch64-linux-gnu-}gcc"

echo "=== Building Structor for Ableton Move (ARM64) ==="
echo "Compiler: $CC"

mkdir -p "$OUT_DIR"

# Single-file C compile → shared library
echo "  CC src/structor.c"
$CC -O2 -shared -fPIC -ffast-math \
    -o "$OUT_DIR/move-everything-structor.so" \
    "$SRC_DIR/structor.c" \
    -lm \
    -Wall -Wno-unused-variable

# Copy module files
cp "$PROJECT_DIR/module.json" "$OUT_DIR/"
cp "$PROJECT_DIR/ui_chain.js" "$OUT_DIR/"

echo "=== Build complete: $OUT_DIR/move-everything-structor.so ==="
ls -la "$OUT_DIR/"
