#!/bin/sh
# ptzd supervisor v2.1: keep /tmp/ptzd alive; camera-side dead-man via HEARTBEAT
# command freshness (clock-skew-proof). No --stop-now kill: STOP is instant
# and cross-fd, killing only risks blocking open() loops.
#
# v2.1 (2026-11-02, Lead): /tmp/ptzd.log is now kept bounded at 256 KiB.
# Why: /tmp is a 17 860 kB tmpfs shared with the RUNNING anyka_ipc (4.9 MB) and
# the SD hack; a full /tmp = no shell on the camera. Measured ptzd.log growth
# ~66 kB/h => ~1,6 MB/24 h with no bound (worst seen sibling: app_vg.log 2,8 MB).
# Rotation is done IN PLACE (`tail > .rot` then `cat .rot > log`, never `mv`)
# so the O_APPEND fds of "$BIN" >> "$LOG" stay valid and no sparse hole is left.
BIN=/tmp/ptzd
PIDF=/tmp/ptzd.pid
HB=/tmp/ptzd.hb
LOG=/tmp/ptzd.log
LOGMAX=262144
LOGKEEP=300
LOGTICK=30
DEADMAN=6

# rotate_log: no-op unless LOG is bigger than LOGMAX; keeps the last LOGKEEP lines.
rotate_log() {
  [ -f "$LOG" ] || return 0
  sz=$(wc -c < "$LOG" 2>/dev/null)
  case "$sz" in ''|*[!0-9]*) sz=0 ;; esac
  [ "$sz" -gt "$LOGMAX" ] || return 0
  if tail -n "$LOGKEEP" "$LOG" > "$LOG.rot" 2>/dev/null; then
    cat "$LOG.rot" > "$LOG" 2>/dev/null
  else
    : > "$LOG"
  fi
  rm -f "$LOG.rot"
}

while true; do
  if [ ! -x "$BIN" ]; then sleep 2; continue; fi
  rotate_log
  "$BIN" >> "$LOG" 2>&1 &
  echo $! > "$PIDF"
  tick=0
  while kill -0 "$(cat $PIDF)" 2>/dev/null; do
    sleep 1
    tick=$((tick + 1))
    if [ "$tick" -ge "$LOGTICK" ]; then
      tick=0
      rotate_log
    fi
    now=$(date +%s)
    hb=$(cat "$HB" 2>/dev/null || echo 0)
    if [ "$now" -gt 0 ] && [ $((now - hb)) -gt "$DEADMAN" ]; then
      "$BIN" --stop-now >> "$LOG" 2>/dev/null
      # stop-now may block on open() while moving; run in bg, keep supervisor alive
      ( sleep 10; kill -9 "$(cat $PIDF)" 2>/dev/null ) &
      KILLER=$!
      break
    fi
  done
  wait 2>/dev/null
  sleep 1
done
