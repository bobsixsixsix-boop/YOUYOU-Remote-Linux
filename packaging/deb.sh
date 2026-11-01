#!/bin/sh
# Build youyou-linux_<version>_arm64.deb from a finished build, on the build machine (the Y700):
#   build/libyouyou_core.so   (cmake -S core -B build)
#   app/target/release/youyou (YY_CORE_LIB_DIR=build cargo build --release in app/)
# Layout: /usr/lib/youyou-linux/{youyou,libyouyou_core.so} (the binary finds the library by its
# $ORIGIN rpath), /usr/bin/youyou -> ../lib/youyou-linux/youyou, youyou-linux.desktop, the icon,
# and /usr/share/doc/youyou-linux/{copyright,README.md}. Installs over 0.1.15 (same package name).
# Usage: sh packaging/deb.sh <out_dir>
set -eu
cd "$(dirname "$0")/.."
out=${1:?usage: deb.sh <out_dir>}
bin=app/target/release/youyou core=build/libyouyou_core.so
[ -x "$bin" ] && [ -f "$core" ] || { echo "deb.sh: build first ($bin, $core)" >&2; exit 1; }
version=$(sed -n 's/^version = "\(.*\)"/\1/p' app/Cargo.toml | head -1)
arch=$(dpkg --print-architecture)
stage=$(mktemp -d)
trap 'rm -rf -- "$stage"' EXIT
chmod 755 "$stage" # mktemp's 0700 would be the package's ./ entry
mkdir "$stage/DEBIAN" # dpkg-shlibdeps resolves $ORIGIN against the tree that has it

lib=$stage/usr/lib/youyou-linux doc=$stage/usr/share/doc/youyou-linux
install -Dm755 "$bin" "$lib/youyou"
install -Dm644 "$core" "$lib/libyouyou_core.so"
strip --strip-unneeded "$lib/youyou" "$lib/libyouyou_core.so"
mkdir -p "$stage/usr/bin" && ln -s ../lib/youyou-linux/youyou "$stage/usr/bin/youyou"
install -Dm644 packaging/youyou-linux.desktop "$stage/usr/share/applications/youyou-linux.desktop"
install -Dm644 assets/launcher.svg "$stage/usr/share/icons/hicolor/scalable/apps/youyou-linux.svg"
install -Dm644 assets/youyou-linux.png "$stage/usr/share/icons/hicolor/256x256/apps/youyou-linux.png"
install -Dm644 README.md "$doc/README.md"
python3 packaging/licenses.py "$doc/copyright"

# Shared-library versions from the binaries themselves; SDL opens EGL/GLES at run time, the
# interface runs fc-match and needs a CJK font, libcurl needs the CA bundle.
mkdir -p "$stage/debian" && printf 'Source: youyou-linux\n\nPackage: youyou-linux\nArchitecture: any\n' > "$stage/debian/control"
shlibs=$(cd "$stage" && dpkg-shlibdeps -O -l"$PWD/usr/lib/youyou-linux" usr/lib/youyou-linux/youyou usr/lib/youyou-linux/libyouyou_core.so | sed -n 's/^shlibs:Depends=//p')
rm -r "$stage/debian"
[ -n "$shlibs" ] || { echo "deb.sh: dpkg-shlibdeps found nothing" >&2; exit 1; }
size=$(du -sk --exclude=DEBIAN "$stage" | cut -f1)
cat > "$stage/DEBIAN/control" <<EOF
Package: youyou-linux
Version: $version
Architecture: $arch
Section: net
Priority: optional
Installed-Size: $size
Maintainer: YOUYOU Remote Linux <noreply@localhost>
Depends: $shlibs, libegl1, libgles2, fontconfig, fonts-noto-cjk, ca-certificates
Homepage: https://github.com/bobsixsixsix-boop/YOUYOU-Remote-Linux
Description: Unofficial UU Remote client for Linux tablets
 Streams a Windows PC running UU Remote (GameViewer) to a Linux tablet:
 V4L2 hardware decoding, frame pacing, touch, mouse, keyboard and
 controller input. Built for the Lenovo Legion Y700 on Ubuntu 26.04.
EOF
mkdir -p "$out"
dpkg-deb --root-owner-group -Zxz --build "$stage" "$out/youyou-linux_${version}_$arch.deb"
