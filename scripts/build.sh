#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
SRC_DIR="$PROJECT_DIR/src"
OUT_DIR="$PROJECT_DIR/dist/structor"

# Cross-compiler prefix (Docker or local toolchain)
CC="${CROSS_PREFIX:-aarch64-linux-gnu-}gcc"

echo "=== Building Structor for Ableton Move (ARM64) ==="
echo "Compiler: $CC"

mkdir -p "$OUT_DIR"

# Compile pffft + fftpack + structor → shared library
echo "  CC src/fftpack.c src/pffft.c src/structor.c"
$CC -O2 -shared -fPIC -ffast-math \
    -o "$OUT_DIR/structor.so" \
    "$SRC_DIR/fftpack.c" \
    "$SRC_DIR/pffft.c" \
    "$SRC_DIR/structor.c" \
    -lm \
    -Wall -Wno-unused-variable -Wno-unknown-pragmas

# Copy module files
cp "$PROJECT_DIR/module.json" "$OUT_DIR/"
if [ -f "$PROJECT_DIR/ui_chain.js" ]; then
    cp "$PROJECT_DIR/ui_chain.js" "$OUT_DIR/"
fi
if [ -f "$PROJECT_DIR/help.json" ]; then
    cp "$PROJECT_DIR/help.json" "$OUT_DIR/"
fi

echo "=== Build complete: $OUT_DIR/structor.so ==="
ls -la "$OUT_DIR/"
