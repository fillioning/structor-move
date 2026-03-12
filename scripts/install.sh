#!/bin/bash
# Install Structor module to Ableton Move via SSH
set -e

MOVE_IP="${1:-move.local}"
MODULE_DIR="$(cd "$(dirname "$0")/.." && pwd)/dist/move-everything-structor"

if [ ! -f "$MODULE_DIR/move-everything-structor.so" ]; then
    echo "Error: move-everything-structor.so not found. Run build.sh first."
    exit 1
fi

echo "Installing Structor to $MOVE_IP..."
DEST="/data/UserData/move-anything/modules/audio_fx/move-everything-structor"
ssh root@$MOVE_IP "mkdir -p $DEST"
scp "$MODULE_DIR/move-everything-structor.so" "$MODULE_DIR/module.json" "$MODULE_DIR/ui_chain.js" root@$MOVE_IP:$DEST/
ssh root@$MOVE_IP "chown -R ableton:users $DEST"

echo "Done. Restart Move-Anything to load Structor."
