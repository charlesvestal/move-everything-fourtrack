#!/usr/bin/env bash
# Build Four Track module for Move Anything (ARM64)
#
# Automatically uses Docker for cross-compilation if needed.
# Set CROSS_PREFIX to skip Docker (e.g., for native ARM builds).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-builder"

# Check if we need Docker
if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== Four Track Module Build (via Docker) ==="
    echo ""

    # Build Docker image if needed
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
        echo ""
    fi

    # Run build inside container
    echo "Running build..."
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh

    echo ""
    echo "=== Done ==="
    exit 0
fi

# === Actual build (runs in Docker or with cross-compiler) ===
CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"

cd "$REPO_ROOT"

echo "=== Building Four Track Module ==="
echo "Cross prefix: $CROSS_PREFIX"

# Create build directories
mkdir -p build
mkdir -p dist/fourtrack

# Compile DSP plugin
echo "Compiling DSP plugin..."
${CROSS_PREFIX}gcc -Ofast -shared -fPIC \
    -march=armv8-a -mtune=cortex-a72 \
    -fomit-frame-pointer -fno-stack-protector \
    -DNDEBUG \
    src/dsp/fourtrack.c \
    -o build/dsp.so \
    -Isrc/dsp \
    -lm -ldl

# Copy files to dist (use cat to avoid ExtFS issues with Docker)
echo "Packaging..."
cat src/module.json > dist/fourtrack/module.json
cat src/ui.js > dist/fourtrack/ui.js
cat build/dsp.so > dist/fourtrack/dsp.so
chmod +x dist/fourtrack/dsp.so

# Create tarball for release
echo "Creating tarball..."
cd dist
tar -czvf fourtrack-module.tar.gz fourtrack/
cd ..

echo ""
echo "=== Build Complete ==="
echo "Output: dist/fourtrack/"
echo "Tarball: dist/fourtrack-module.tar.gz"
echo ""
echo "To install on Move:"
echo "  ./scripts/install.sh"
