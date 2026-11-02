#!/bin/sh
# Build youyou-linux-<version>.tar.xz, the complete source for a release: the tree exported
# from the release tag, the upstream's pinned native dependencies (core/upstream/external, as
# fetch_deps.py + patch_deps.py leave them; both scripts skip work that is already done) and
# the Rust crates in vendor/ with .cargo/config.toml pointing at them, so it builds offline.
# Run on the build machine in a tree exported with git archive, after fetch_deps/patch_deps.
# cargo vendor takes every platform's crates, so it may download from crates.io.
# Usage: sh packaging/source.sh <out_dir>
set -eu
cd "$(dirname "$0")/.."
out=${1:?usage: source.sh <out_dir>}
[ -d core/upstream/external ] || { echo "source.sh: run fetch_deps.py and patch_deps.py first" >&2; exit 1; }
[ ! -e .git ] || { echo "source.sh: run in a git archive export, not a checkout" >&2; exit 1; }
version=$(sed -n 's/^version = "\(.*\)"/\1/p' app/Cargo.toml | head -1)
name=youyou-linux-$version
stage=$(mktemp -d)
trap 'rm -rf -- "$stage"' EXIT
mkdir "$stage/$name"
tar --exclude=./build --exclude=./out --exclude=./vendor --exclude=./.cargo \
    --exclude=./app/target --exclude=./account/target --exclude=./account/Cargo.lock -cf - . | tar -C "$stage/$name" -xf -
(cd app && cargo vendor --locked --quiet "$stage/$name/vendor" >/dev/null)
mkdir "$stage/$name/.cargo"
# Relative to the directory holding .cargo/, so app/ and account/ both find it.
printf '[source.crates-io]\nreplace-with = "vendored-sources"\n\n[source.vendored-sources]\ndirectory = "vendor"\n' \
    > "$stage/$name/.cargo/config.toml"
mkdir -p "$out"
tar -C "$stage" --sort=name --owner=0 --group=0 --numeric-owner -cJf "$out/$name.tar.xz" "$name"
