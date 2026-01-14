#!/usr/bin/env bash
# Install Four Track module to Move device
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

# Move connection settings
MOVE_HOST="${MOVE_HOST:-move.local}"
MOVE_USER="${MOVE_USER:-root}"
MOVE_MODULES_DIR="/data/UserData/move-anything/modules"

echo "=== Installing Four Track Module ==="
echo "Target: $MOVE_USER@$MOVE_HOST"

# Check if dist exists
if [ ! -d "$REPO_ROOT/dist/fourtrack" ]; then
    echo "Error: dist/fourtrack not found. Run ./scripts/build.sh first."
    exit 1
fi

# Create module directory on Move
echo "Creating module directory..."
ssh "$MOVE_USER@$MOVE_HOST" "mkdir -p $MOVE_MODULES_DIR/fourtrack"

# Copy files
echo "Copying files..."
scp -r "$REPO_ROOT/dist/fourtrack/"* "$MOVE_USER@$MOVE_HOST:$MOVE_MODULES_DIR/fourtrack/"

# Install Line In patch to chain/patches
echo "Installing Line In patch..."
ssh "$MOVE_USER@$MOVE_HOST" "mkdir -p $MOVE_MODULES_DIR/chain/patches"
scp "$REPO_ROOT/src/patches/linein.json" "$MOVE_USER@$MOVE_HOST:$MOVE_MODULES_DIR/chain/patches/"

echo ""
echo "=== Installation Complete ==="
echo "Module installed to: $MOVE_MODULES_DIR/fourtrack"
echo ""
echo "Restart Move Anything to load the new module."
