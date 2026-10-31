#!/bin/sh
set -eu
upstream=$1
share=$2
for font in NotoSansSC-Regular.otf NotoSansSC-Bold.otf OFL.txt; do
    install -Dm644 "$upstream/assets/fonts/$font" "$share/fonts/$font"
done
install -Dm644 assets/youyou-mark.rgba "$share/ui-tv/youyou-mark.rgba"
install -Dm644 assets/youyou-mark-192.rgba "$share/ui-tv/youyou-mark-192.rgba"
for img in mouse touch; do
    install -Dm644 "$upstream/assets/$img.rgba" "$share/ui-tv/$img.rgba"
done
python3 native/render_icons.py assets "$share/ui-svg"
