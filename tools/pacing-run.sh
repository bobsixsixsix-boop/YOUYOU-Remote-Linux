#!/bin/bash
# Runs on the Y700 as root: what reaches the panel (KWin flips, via y700's
# linux/streaming/ml-flips.py) while `youyou play` shows a 120 fps HEVC
# 2560x1600 clip, per panel rate (164/120 Hz) and vsync. Needs the clips in
# /tmp/yydec (hevc-2560.hevc) and the build in /root/yybuild. Restores 164 Hz.
# y700-refresh-auto takes each kscreen-doctor switch as a new base rate; ending on
# 164 puts its base back. Do not run it while someone uses the tablet: 164<->120
# is a full modeset (the screen blinks) and shakes out ghost touches.
set -u
export LC_ALL=C
as_user() { runuser -u pocketds -- env -i HOME=/home/pocketds PATH=/usr/bin:/bin XDG_RUNTIME_DIR=/run/user/1000 \
  WAYLAND_DISPLAY=wayland-0 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus "$@"; }
trap 'as_user kscreen-doctor output.DSI-1.mode.2 >/dev/null 2>&1' EXIT
cd /tmp/yydec
[ -f long-2560.hevc ] || for i in 1 2 3 4 5 6 7 8; do cat hevc-2560.hevc; done > long-2560.hevc
ffmpeg -loglevel error -y -f lavfi -i smptebars=size=1920x1080:rate=120 -t 3 -c:v libx264 -preset fast -bf 0 -g 120 \
  -pix_fmt yuv420p -color_primaries bt709 -color_trc bt709 -colorspace bt709 bars-1080.h264
rm -rf /tmp/yyplay && mkdir -p /tmp/yyplay
cp /root/yybuild/app/target/release/youyou /root/yybuild/build/libyouyou_core.so /tmp/yyplay/
chown -R pocketds: /tmp/yyplay
for run in "164 1" "164 0" "120 1" "120 0"; do
  set -- $run
  [ $1 = 120 ] && as_user kscreen-doctor output.DSI-1.mode.1 || as_user kscreen-doctor output.DSI-1.mode.2
  sleep 2
  echo "===== panel ${1} Hz, vsync=$2"
  as_user env YY_VSYNC=$2 timeout 40 /tmp/yyplay/youyou play /tmp/yydec/long-2560.hevc 120 > /tmp/yyplay/app-$1-$2.txt 2>&1 &
  app=$!
  sleep 5
  python3 /root/streaming/ml-flips.py 15 /tmp/yyplay/flips-$1-$2.txt 2>&1 | tail -n 30
  wait $app
  grep -v "^libGL" /tmp/yyplay/app-$1-$2.txt | tail -n 8
done
echo "===== bars-1080 (-bf 0), panel 164 Hz"
as_user kscreen-doctor output.DSI-1.mode.2; sleep 2
as_user env YY_VSYNC=1 timeout 30 /tmp/yyplay/youyou play /tmp/yydec/bars-1080.h264 120 2>&1 | grep -v "^libGL"
