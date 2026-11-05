# Install — SD-only mode (no PC)

> **Status: experimental in v0.1.** The mechanism is in place and the card-side
> script does the work on the camera, but this path has not yet gone through a full
> validation campaign. Mode B ([card-free](install-pc.md)) is the proven one.

Idea: the card **stays in the camera** and carries everything. At every boot the
vendor firmware runs our bootstrap script from the card, which:

1. gives the camera its factory test mode (RTSP up),
2. copies our two daemons into RAM and starts them.

No PC, no flashing, no partition rewrite. Unplug the card and you are back to a stock
camera (its jffs2 hooks stay behind, see [rollback](rollback.md)).

## Steps

```bash
sudo python3 installer/sd_card.py \
    --ssid "YourWiFi" --pass "YourWiFiPassword" \
    --ip 192.168.1.50 --netmask 255.255.255.0 --gateway 192.168.1.1 \
    --mode-a --out /media/$USER/CARD
```

Insert the card, power on, wait ~60 s. Then:

* `rtsp://192.168.1.50:554/main_ch` should be alive,
* `http://192.168.1.50:8080/healthz` should answer.

To see what happened, read the card from a PC afterwards:

| File on the card | Meaning |
|---|---|
| `logs/READY.txt` | what is listening now, marker and shell status |
| `logs/identity.txt` | firmware build, md5 of the vendor binaries |
| `logs/hack.log` | bootstrap trace and warnings |
| `logs/modeA.log` | daemon start trace |

## Why the card cannot be removed in this mode

Our daemons total ~600 KiB and the writable flash partition (`/etc/config`, jffs2) has
only ~24 KiB free. They therefore live in RAM (`/tmp`), which is wiped at every boot,
and the card is what puts them back. If you want the card out, use the
[card-free mode](install-pc.md).

## Caveats

* The card must be a normal FAT32 card the vendor firmware accepts.
* Reports on the card are appended at every boot; delete `logs/` if they grow.
* `MODE_A` on the card is the flag that enables the daemon start-up. Remove it to keep
  only the factory-mode part.
