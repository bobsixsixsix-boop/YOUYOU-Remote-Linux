#!/bin/bash
# Runs on the Y700 as root (via systemd-run, so no ssh session during the measurement;
# tools/desktop-run.py starts it): youyou connect <pc> fullscreen with YY_ENV, ml-flips.py
# in the steady middle, then SIGTERM -> SDL Quit
# -> the player disconnects. SIGINT would not do: bash leaves it ignored for background
# jobs, so SDL never installs its handler. config.ini is shared with v0.1.15 and
# quality-settings rewrites it, so it is put back afterwards.
# Usage: stream-run.sh <pc> <label> [settle_s] [measure_s] [hold_s]
# hold_s: after the measurement, wait that long for the PC side to end the session.
set -u
export LC_ALL=C
PC=$1; OUT=/tmp/yyrun/$2; SETTLE=${3:-20}; SECS=${4:-30}; HOLD=${5:-0}
STATE=/home/pocketds/.local/state/youyou-linux
rm -rf $OUT && mkdir -p $OUT /tmp/yyplay
cp -p $STATE/config.ini $OUT/config.ini.before
cp /root/yybuild/app/target/release/youyou /root/yybuild/build/libyouyou_core.so /tmp/yyplay/
chown -R pocketds: /tmp/yyplay
runuser -u pocketds -- env -i HOME=/home/pocketds PATH=/usr/bin:/bin XDG_RUNTIME_DIR=/run/user/1000 \
  WAYLAND_DISPLAY=wayland-0 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus ${YY_ENV:-} \
  /tmp/yyplay/youyou connect "$PC" > $OUT/app.txt 2>&1 &
sleep "$SETTLE"
python3 /root/streaming/ml-flips.py "$SECS" $OUT/flips-detail.txt > $OUT/flips.txt 2>&1
for i in $(seq $((HOLD * 5))); do pgrep -f "^/tmp/yyplay/youyou connect" >/dev/null || break; sleep 0.2; done
pkill -TERM -f '^/tmp/yyplay/youyou connect'
for i in $(seq 50); do pgrep -f '^/tmp/yyplay/youyou connect' >/dev/null || break; sleep 0.2; done
pgrep -f '^/tmp/yyplay/youyou connect' >/dev/null && { echo "still running, SIGKILL" > $OUT/killed; pkill -KILL -f '^/tmp/yyplay/youyou connect'; }
cp -p $OUT/config.ini.before $STATE/config.ini
echo done > $OUT/done
