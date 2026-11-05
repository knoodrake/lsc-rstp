# Install — access-point provisioning (planned)

> **Status: components built and executed on the camera, not wired yet.**

The goal is the phone-only experience: the camera boots as a WiFi access point, you
join it, a page asks for your home WiFi credentials, the camera reboots on your
network. No PC, no card reader.

## What already exists

| Piece | State |
|---|---|
| `hostapd` 2.11 for ARMv5/uClibc | **built and run on the camera** |
| setup web page (`apweb`, 66 KiB, libc only) | **built and run on the camera**: form, validation, atomic write of the factory marker, reboot on save |
| radio driver in AP mode | the driver is standard `mac80211` (`driver=nl80211` works); the AP switch script ships in the firmware, **not exercised yet** |
| rollback if the radio does not come back | designed, not implemented |

## What is missing

1. switch the radio to AP (`wifi_driver_rtl8188.sh ap`, address `172.14.10.1`),
2. run `udhcpd` + `hostapd` + `apweb` in that order with proper supervision,
3. handle "user gives wrong WiFi password" (re-open the AP after a timeout),
4. only then: package it so that a card or the baked layer can start it.

Nothing here needs `mksquashfs`; the whole feature lives in RAM. Contribute if you
want it sooner.
