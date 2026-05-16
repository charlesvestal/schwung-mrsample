#!/usr/bin/env bash
# Build mrsample module for Schwung (ARM64)
# Uses Docker for cross-compilation unless CROSS_PREFIX is provided.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-builder"

if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== mrsample Build (via Docker) ==="
    echo ""

    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
        echo ""
    fi

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

CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"

cd "$REPO_ROOT"

echo "=== Building mrsample Module ==="
echo "Cross prefix: $CROSS_PREFIX"

MODULE_DIR="dist/mrsample"
TARBALL="dist/mrsample-module.tar.gz"

mkdir -p build dist
rm -rf "$MODULE_DIR"
mkdir -p "$MODULE_DIR"

echo "Compiling DSP plugin..."
${CROSS_PREFIX}g++ -g -O3 -shared -fPIC -std=c++14 \
    src/dsp/mrsample_plugin.cpp \
    src/dsp/mrsample_engine.cpp \
    src/dsp/mrsample_params.cpp \
    -o build/dsp.so \
    -Isrc/dsp \
    -lm -lpthread

echo "Packaging..."
cat src/module.json > "$MODULE_DIR/module.json"
cat build/dsp.so > "$MODULE_DIR/dsp.so"
chmod +x "$MODULE_DIR/dsp.so"
cat README.md > "$MODULE_DIR/README.md"
cat src/help.json > "$MODULE_DIR/help.json"

rm -f "$TARBALL"
(
  cd dist
  tar -czf "$(basename "$TARBALL")" "$(basename "$MODULE_DIR")"
)

echo ""
echo "=== Build Complete ==="
echo "Output: $MODULE_DIR/"
echo "Tarball: $TARBALL"
echo ""
echo "To install on Move:"
echo "  ./scripts/install.sh"
