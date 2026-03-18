#!/bin/bash
# Install Structor module to Ableton Move via SSH
set -e

MOVE_IP="${1:-move.local}"
MODULE_DIR="$(cd "$(dirname "$0")/.." && pwd)/dist/structor"

if [ ! -f "$MODULE_DIR/structor.so" ]; then
    echo "Error: structor.so not found. Run build.sh first."
    exit 1
fi

echo "Installing Structor to $MOVE_IP..."
DEST="/data/UserData/move-anything/modules/audio_fx/structor"
ssh root@$MOVE_IP "mkdir -p $DEST"
scp "$MODULE_DIR/structor.so" "$MODULE_DIR/module.json" "$MODULE_DIR/ui_chain.js" "$MODULE_DIR/help.json" root@$MOVE_IP:$DEST/
ssh root@$MOVE_IP "chown -R ableton:users $DEST"

echo "Done. Restart Move-Anything to load Structor."
