#!/bin/sh
set -eu
cd "$(dirname "$0")"
src=$(pwd)
build=${BUILD_DIR:-"$src/build"}
mkdir -p "$build"
build=$(cd "$build" && pwd)
if [ -d "$src/vendor/upstream" ]; then
    default_upstream="$src/vendor/upstream"
else
    default_upstream="$build/upstream"
fi
upstream=${UU_UPSTREAM:-"$default_upstream"}
python3 native/prepare.py --output "$upstream"
cmake -S native -B "$build/native" -G Ninja -DCMAKE_BUILD_TYPE=Release -DUU_UPSTREAM="$upstream"
cmake --build "$build/native" --target youyou_core --parallel "${JOBS:-4}"
export YOUYOU_NATIVE_DIR="$build/native"
export CARGO_TARGET_DIR="$build/cargo"
cargo build --release --locked
echo "Built $CARGO_TARGET_DIR/release/youyou-linux"
if [ "${1:-}" = "--deb" ]; then
    sh packaging/deb.sh "$CARGO_TARGET_DIR/release/youyou-linux" "$build/native/libyouyou_core.so" "$upstream" "$(dpkg --print-architecture)" "$build"
else
    sh packaging/install-assets.sh "$upstream" "$build/assets"
    printf 'Run with YOUYOU_ASSET_DIR="%s" LD_LIBRARY_PATH="%s" "%s"\n' "$build/assets" "$build/native" "$CARGO_TARGET_DIR/release/youyou-linux"
fi
