# AP-mode provisioning (phone-only setup)

> **Status: the access point itself is proven on real hardware; the card flow is not yet
> tested on a physical card.** Read [Status](#status) before relying on it.

The goal is the no-PC experience: the camera boots as a WiFi access point, you join it from
a phone, a page asks for your home WiFi name/password, the camera reboots on your network.

This folder is the whole feature. It needs no flash write: everything runs from RAM and
the card is only ever *read*.

## Status

Proven on a real camera (cam_3, 2026-11-06):

```
iwconfig wlan0   ESSID:"LSC-SETUP"  Mode:Master  Frequency:2.437 GHz
ifconfig wlan0   inet addr:172.14.10.1
hostapd          wlan0: interface state UNINITIALIZED->ENABLED / AP-ENABLED
ps               udhcpd /etc/config/udhcpd.conf -S
ping 172.14.10.1 0% packet loss
GET  /           200 OK  (setup form)
POST /save       200 OK  + factory INI written in the exact format the firmware reads
```

Returning to station mode is proven both ways: `wpa_supplicant` + static address
(camera answers again on its old IP) and a plain reboot.

**Known limitation:** the AP window does not currently hold for more than ~20–40 s. The
firmware's own network manager (`anyka_ipc`) restarts `wpa_supplicant` and puts the radio
back into station mode. `enter_ap.sh` fights this with a keep-alive loop
(`maintien : AP perdu -> relance hostapd`), but that part still needs tuning. A reboot is
never harmful: the card stays in place and the camera boots normally.

## Why the vendor firmware already knows about this

`/etc/config/udhcpd.conf` hardcodes `172.14.10.1`, and `/usr/sbin/wifi_driver_rtl8188.sh ap`
does:

```sh
ifconfig wlan0 172.14.10.1
udhcpd /etc/config/udhcpd.conf -S
/tmp/hostapd /etc/config/hostapd.conf -B
```

What is missing is **the hostapd binary**: the firmware has none and expects
`/mnt/hostapd` from the SD card. The vendor `service.sh` even copies it:

```sh
AP_MODE_FILE=_ht_ap_mode.conf
dd if="/mnt/${AP_MODE_FILE}" of="/tmp/${AP_MODE_FILE}" bs=1024 count=1
if test -f "/tmp/${AP_MODE_FILE}" ; then
    cp "/tmp/${AP_MODE_FILE}" /etc/config/hostapd.conf -rf
    cp /mnt/hostapd /tmp/
fi
```

So the vendor's provisioning card is `_ht_ap_mode.conf` + `hostapd`. This folder supplies
both (we ship our own hostapd build, `out/hostapd`, ARMv5/uClibc, `driver=nl80211`).

## Make the card

```sh
python3 make_kit.py /media/<card>     # FAT32, files copied to the root
```

That writes `hack.sh`, `enter_ap.sh`, `hostapd`, `hostapd.bin`, `apweb`, `shadow` and a
`README.txt` for the end user. Boot the camera with the card in.

The camera then raises **`LSC-SETUP`** (open) and serves `http://172.14.10.1/`; the page
writes `/etc/config/_ak39_factory.ini` and reboots. No answer within 15 minutes → the
camera reboots back to normal. Last run log: `/etc/config/lsc_ap_last.log`.

## Files

| File | Role |
|---|---|
| `out/hostapd` | our hostapd 2.11 build, ARMv5/uClibc, dynamic (`libc.so.0` only) |
| `out/apweb` | setup web server (libc only, no busybox httpd on these builds) |
| `launcher/hostapd` | compatibility wrapper (see pitfall 1) |
| `enter_ap.sh` | AP + `apweb` + timeout + reboot, keep-alive loop, dead-man switch |
| `sd/hack.sh` | card bootstrap: copies the kit to `/tmp`, waits for the radio, starts `enter_ap.sh` |
| `make_kit.py` | assembles a ready-to-copy card |
| `build.sh`, `build_apweb.sh`, `uclibc_compat.c` | reproducible builds (tarballs fetched if absent) |

## Three pitfalls found the hard way

1. **`hostapd -B` no longer exists** (removed after 2.6). The vendor script passes `-B`,
   and hostapd ≥ 2.7 takes it *for the configuration file*:
   `Could not open configuration file '-B'`.
   Fix: `launcher/hostapd` strips `-B`, logs to `/tmp/hostapd.out` and execs the real
   binary (`/tmp/hostapd.bin`, falling back to `/mnt/hostapd.bin`).
2. **There is no `tr` on the camera's busybox** (`tr: not found`). Use `sed`, `head`,
   `cut`, `awk` in anything that runs on the camera.
3. **`apweb --self-test` must never write the production INI.** It used to overwrite
   `/etc/config/_ak39_factory.ini`; it now writes `/tmp/apweb_selftest.ini`
   (`APWEB_OUT=<path>` to choose).

## Building from source

```sh
bash build.sh        # hostapd 2.11 + static libnl3, uClibc crt0, 7 documented pitfalls
bash build_apweb.sh  # setup server, host build (tests) and ARM build
```

Requires the `arm-linux-gnueabi` toolchain (Ubuntu package `gcc-arm-linux-gnueabi`).
Both binaries only need `libc.so.0` on the camera.
