# Architecture

## The platform

Anyka AK3918EV330 / AK3168E: ARM926EJ-S (ARMv5TEJ), Linux 4.4, uClibc, busybox.
8 MiB of SPI NOR, partitioned roughly as bootloader, kernel, rootfs (read-only,
squashfs) and an **application partition (`/dev/mtd7`, 5 MiB, squashfs)** that holds
the vendor program `anyka_ipc` and its resource tree.

`/usr` is that application partition, mounted read-only and **100 % full**.
`/etc/config` is a small writable jffs2 partition (~256 KiB, ~24 KiB free).
`/tmp` is a 16 MiB tmpfs wiped at boot.

## How a stock camera becomes a local camera

The vendor program contains a hidden **factory test mode**: when the file
`/tmp/_ak39_factory.ini` exists at the moment `anyka_ipc` starts, it opens an RTSP
server on port **554** and never talks to Tuya again. That file is normally copied
from an SD card by the vendor's own `service.sh`.

We exploit exactly that, and nothing more:

```
SD card  ──▶  service.sh (vendor)  ──▶  /tmp/_ak39_factory.ini  ──▶  RTSP :554
                                            ▲
                                            └── our fallback when no card is present
```

After the bake, our layer provides that file from `/etc/config` (jffs2) or from
`/usr/share/lsc` inside the application partition, so the mode survives without any
external media.

## Our layer (the only thing we add)

| File | Role |
|---|---|
| `sbin/service.sh` (patched) | generic hook inserted before `anyka_ipc.sh start`: restore the marker, start our boot loop, keep a root shell alive |
| `share/lsc/lsc_boot.sh` | boot loop: heals the marker, respawns the root shell, restarts the ONVIF daemon if it dies |
| `share/lsc/_ak39_factory.ini` | fallback copy of the factory marker (WiFi + static IP, taken from your own camera) |
| `sbin/ptzd` + `sbin/ptzd-supervise.sh` | TCP server that owns the PTZ motor (position, moves, stop) |
| `sbin/lsc_onvif` | ONVIF PTZ server: SOAP on `:8080`, translates ONVIF into `ptzd` commands |

Nothing of the vendor program is modified or replaced, which is why the same layer
works on every build: the installer builds the image **from the camera's own dump**.

## Why a separate PTZ daemon

The vendor program exposes no PTZ API locally. `ptzd` speaks to the motor driver,
keeps the current position and implements relative moves in **closed loop** (move,
read position, correct) with a tolerance of 8 motor steps. `lsc_onvif` is a small
stateless SOAP server in front of it.

There is no `AbsoluteMove` on this hardware; it is emulated with bounded relative
moves.

## The tilt axis trap (important for NVR integration)

The motor angle is not the ONVIF angle:

* raw position `0` = looking at the floor, `≈1024` = level, `2048` = ceiling;
* `home` (`y=0` in the old mapping) is the **folded privacy position**, i.e. the ceiling;
* the direction of positive `y` differs between the motor and the ONVIF expectation
  (Frigate sends `tilt > 0` to move **up**).

`lsc_onvif` therefore calibrates the tilt against the **`level` preset**: recording or
recalling `level` sets the zero point, so `y = 0` always means "the standby view you
chose", even after the motor counter drifts following a mechanical stop.

## Ports

| Port | Service |
|---|---|
| 554 | RTSP (`/main_ch`, `/videoSub` when available) |
| 8080 | ONVIF PTZ + `/healthz` |
| 8090 | HUTO (vendor audio protocol) |
| 8091 | `ptzd` (local only, single connection) |
| 23 / 24 | vendor telnet / our root shell (direct, no login) |
