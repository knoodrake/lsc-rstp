# Requirements

## Hardware

* An **LSC / Tuya PTZ camera** on the Anyka **AK3918EV330 / AK3168E** platform.
  Verified on three units (see the compatibility table in the README).
* A **microSD card**, 32 GB or less, **FAT32**. Capacity does not matter; 1 GB is plenty.
* A **USB card reader** (or any way to write the card from your PC).

## Network

* The camera and your PC on the **same LAN**, with a **static IP** plan: in factory
  test mode the firmware does **not** run a DHCP client (verified: `udhcpc` exists but
  is never started). You must decide an address and put it on the card.
* Nothing is required from the internet. After installation the camera never needs to
  reach Tuya again.

## PC side (card-free install only)

* **Linux** (any distribution; macOS works too if you install `squashfs-tools`).
* **Python 3.9+** — standard library only, no pip package.
* **squashfs-tools** (`mksquashfs`, `unsquashfs`):
  * Debian/Ubuntu: `sudo apt install squashfs-tools`
  * Fedora: `sudo dnf install squashfs-tools`
  * Arch: `sudo pacman -S squashfs-tools`
* **telnet** client (`telnet` or `nc`) to reach the camera's root shell.

## Optional

* **Frigate** (tested with the `0.18` series) — for detection, recording, autotracking.
* **Home Assistant** — if you drive the camera through MQTT or ONVIF.

## What you do NOT need

* No Tuya account, no vendor app, no cloud.
* No SDK, no compiler: the ARM binaries are prebuilt in `bin/` and `layer/`.
  (You only need a toolchain if you want to rebuild them — see `src/`.)
* No serial adapter, no soldering, no disassembly.
