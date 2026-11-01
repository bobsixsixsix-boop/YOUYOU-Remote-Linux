#!/bin/sh
set -eu
if [ "$#" -ne 5 ]; then
    echo "Usage: deb.sh /path/to/youyou-linux /path/to/libyouyou_core.so /path/to/upstream arm64 /out" >&2
    exit 2
fi
bin=$1
core=$2
upstream=$3
arch=$4
out=$5
version=$(python3 -c 'import tomllib; print(tomllib.load(open("Cargo.toml", "rb"))["package"]["version"])')
mkdir -p "$out"
stage=$(mktemp -d)
trap 'rm -rf -- "$stage"' EXIT
share="$stage/usr/share/youyou-linux"
doc="$stage/usr/share/doc/youyou-linux"
install -Dm755 "$bin" "$stage/usr/bin/youyou-linux"
install -Dm644 "$core" "$stage/usr/lib/youyou-linux/libyouyou_core.so"
sh packaging/install-assets.sh "$upstream" "$share"
install -Dm755 integrations/y700-edge/enable.py "$stage/usr/bin/youyou-enable-y700-gestures"
install -Dm644 integrations/y700-edge/stream-edges.js "$share/y700-edge/stream-edges.js"
install -Dm644 packaging/youyou-linux.desktop "$stage/usr/share/applications/youyou-linux.desktop"
install -Dm644 assets/youyou-linux.png "$stage/usr/share/icons/hicolor/256x256/apps/youyou-linux.png"
install -Dm644 README.md "$doc/README.md"
install -Dm644 docs/home.webp "$doc/docs/home.webp"
python3 packaging/licenses.py --upstream "$upstream" --output "$doc/copyright"
mkdir -p "$stage/DEBIAN"
cat > "$stage/DEBIAN/control" <<EOF
Package: youyou-linux
Version: $version
Architecture: $arch
Section: net
Priority: optional
Maintainer: YOUYOU Linux port <noreply@localhost>
Depends: libc6 (>= 2.43), libstdc++6, libsdl2-2.0-0, libsdl2-ttf-2.0-0, libavcodec62, libavutil60, libswscale9, libva2, libegl1, libgles2, libopus0, libcurl4t64, ca-certificates, curl, python3
Recommends: kdialog
Description: Unofficial UU Remote client for Linux
 Rust tablet input and playback with the YOUYOU Remote interface, for Ubuntu 26.04.
EOF
dpkg-deb --root-owner-group -Zxz -z4 --build "$stage" "$out/youyou-linux_${version}_$arch.deb"
