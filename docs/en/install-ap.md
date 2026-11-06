# Install — access-point provisioning (phone-only setup)

> **Status: the access point is proven on a real camera; the card flow is not yet tested on
> a physical card, and the AP window still needs tuning (see limitations below).**

The goal: the camera boots as a WiFi access point, you join it from a phone, a page asks for
your home WiFi name/password, the camera reboots onto your network. No PC, no card reader.

The whole feature lives in **[`ap-mode/`](../../ap-mode/README.md)** — sources, prebuilt
binaries, the card builder (`make_kit.py`) and the camera scripts. Nothing is written to the
flash.

## What is proven (one real camera, 2026-11-06)

| Piece | Evidence |
|---|---|
| Radio in AP mode | `iwconfig`: `ESSID:"LSC-SETUP" Mode:Master Frequency:2.437 GHz` |
| Address + DHCP | `ifconfig`: `172.14.10.1`; `udhcpd /etc/config/udhcpd.conf -S` running |
| hostapd (ours, ARMv5) | `wlan0: interface state UNINITIALIZED->ENABLED` / `AP-ENABLED` |
| Setup page | `GET /` → `200 OK`; `POST /save` → `200 OK` and the factory INI written in the exact format the firmware reads |
| Return to station | `wpa_supplicant` + static address → camera answers on its old IP; a plain reboot works too |

## Why it works at all

The vendor firmware already implements AP mode — it hardcodes `172.14.10.1` in
`/etc/config/udhcpd.conf` and `/usr/sbin/wifi_driver_rtl8188.sh ap` starts `udhcpd` plus
`/tmp/hostapd /etc/config/hostapd.conf -B`. What it never shipped is **the hostapd binary**:
`service.sh` copies it from the card (`cp /mnt/hostapd /tmp/`, next to `_ht_ap_mode.conf`
which becomes `hostapd.conf`). This project supplies that binary, built for the camera's
ARMv5/uClibc with `driver=nl80211` (the WiFi module is a standard `mac80211` device).

## Usage

```sh
python3 ap-mode/make_kit.py /media/<card>   # FAT32 card, files at the root
```

Insert the card, power the camera: it raises **`LSC-SETUP`** (open network) and serves
`http://172.14.10.1/`. Fill in your WiFi, save — the camera reboots onto your network.
No answer within 15 minutes → the camera reboots back to normal by itself.

Diagnostics: `/etc/config/lsc_ap_last.log` on the camera (survives the reboot),
`/tmp/ap.log`, `/tmp/hostapd.out`.

## Limitations

- **The AP window does not hold beyond ~20–40 s yet.** The firmware's network manager
  (`anyka_ipc`) restarts `wpa_supplicant` and returns the radio to station mode; the
  keep-alive loop in `enter_ap.sh` re-launches hostapd, but this part still needs tuning.
  A reboot in the middle is harmless (the card is only read).
- Not yet tested on a **virgin camera** (the P5 target) nor with a **physical card**.
- The page is open-network provisioning on purpose; do not leave the card inserted in
  production (the camera would raise an AP at every boot).

See also: [troubleshooting](troubleshooting.md), [limitations](limitations.md).
