# LSC PTZ → local RTSP + ONVIF PTZ (no cloud)

Turn a cheap **LSC / Tuya PTZ camera** (Anyka AK3918EV330 · AK3168E platform) into a
**fully local camera**: plain RTSP streams and a working **ONVIF PTZ** service, no Tuya
cloud, no vendor app, no account. Built so that **Frigate** (and Home Assistant) can use
PTZ including **autotracking**.

*Read this in: **English** · [Français](README.fr.md)*

> **Status: early preview (v0.1).** Everything below was developed and verified on
> **three physical cameras**. The camera-side software is proven; the *one-command
> installer* is still being packaged. See the [status matrix](#status--what-is-proven).

---

## Why

Tuya PTZ cameras stream through the cloud and expose no usable local PTZ. On the Anyka
platform the hardware already does everything we need — RTSP, an H.264 encoder, a PTZ
motor with position feedback — but the vendor firmware only exposes it in a hidden
**factory test mode**. This project makes that mode permanent, adds an ONVIF daemon on
top of the motor, and gives you a camera that a normal NVR can drive.

## What you get

| | |
|---|---|
| **RTSP** | `rtsp://<cam>:554/main_ch` — H.264 1920×1080 (+ AAC audio) |
| **RTSP sub** | `rtsp://<cam>:554/videoSub` — 640×360, *when the build supports it* (see limitations) |
| **ONVIF PTZ** | `http://<cam>:8080/onvif/ptz_service` — RelativeMove / ContinuousMove / Stop / presets / GetStatus |
| **Health** | `GET http://<cam>:8080/healthz` (JSON, 200 only if the PTZ backend answers) |
| **Frigate** | works with `onvif:` + `autotracking`, including `return_preset` |
| **No cloud** | the camera never needs to reach Tuya again |

## Supported hardware

Verified on three units of the same model family:

| Build in `share/VERSION` | Notes |
|---|---|
| `3.10.62.5` | full feature set, sub-stream available |
| `3.10.56.1146` | main stream + PTZ, sub-stream missing |
| `3.287.75.0` | main stream + PTZ, sub-stream missing |

Other **Anyka AK3918 / AK3168** Tuya PTZ cameras are likely compatible — the installer
dumps *your own* firmware and injects only our layer, so it adapts to your build instead
of shipping a fixed image. Untested models are marked **experimental**.

## Requirements

* the camera, obviously, and a **microSD card** (≤ 32 GB, FAT32) for the first contact
* **a Linux PC** on the same LAN — only for the *card-free* install mode (Python 3.9+,
  `mksquashfs`/`unsquashfs` from `squashfs-tools`, `telnet`)
* optionally **Frigate** and/or **Home Assistant**

## How it works (two install modes)

Both modes start the same way: you write a **probe card**. On boot the vendor firmware
reads it, opens a root shell on port 24 and exposes RTSP — that is your entry point.
From there you pick:

```
        microSD probe card  ──▶  camera boots: RTSP :554 + root shell :24
                                        │
              ┌─────────────────────────┴──────────────────────────┐
              ▼                                                    ▼
  MODE A — SD-only (no PC)                         MODE B — bake (card-free)
  the card also carries our daemons                installer dumps your own APP,
  and starts them at every boot                    injects our layer, reflashes
  card stays in the camera                         mtd7 with md5 verification
  ✔ zero software on any PC                        ✔ card removed afterwards
  ⚠ experimental in v0.1                           ✔ the mode verified on 3 cameras
```

* **[Mode B — card-free install (PC)](docs/en/install-pc.md)** — the recommended path.
* **[Mode A — SD-only install (no PC)](docs/en/install-sd.md)** — experimental.
* **[Mode C — access-point provisioning (phone only)](docs/en/install-ap.md)** — planned;
  `hostapd` and the setup web page already run on the camera.

## Quick start (Mode B)

```bash
git clone https://github.com/LocalActionTuyaCams/lsc-rstp.git
cd lsc-rstp
pip install -r installer/requirements.txt

# 1. build the probe card (writes files to a mounted FAT32 card)
sudo python3 installer/sd_card.py --ssid "YourWiFi" --pass "YourPassword" \
    --ip 192.168.1.50 --netmask 255.255.255.0 --gateway 192.168.1.1 --out /media/card

# 2. insert card, power the camera, wait ~60 s, then verify the entry point
telnet <cam-ip> 24

# 3. bake the camera (dump → inject → flash → md5 readback)
python3 installer/install.py <cam-ip> --apply

# 4. remove the card, reboot, verify
python3 tests/validate_cam.py <cam-ip> 8080
```

## Status — what is proven

| Component | State |
|---|---|
| RTSP `main_ch` (1080p) from factory mode | **verified**, 3 cameras, survives reboot |
| ONVIF PTZ daemon (`lsc_onvif`, in-camera) | **verified** — 22/23-conformance self-tests pass per camera |
| PTZ closed-loop `RelativeMove` + `GetStatus` | **verified**, ±0.06 tolerance in FOV space |
| Presets (`home`, `level`) | works, **not persistent across reboot** (see limitations) |
| Frigate autotracking | **verified** end-to-end once the tilt fix landed |
| Sub-stream `videoSub` | verified on `3.10.62.5`, **missing** on two builds |
| PC bake installer (`install.py`) | **works**, run for real on 2 cameras — being packaged |
| SD-only install | designed, partially implemented, **experimental** |
| AP provisioning | components built and run on-camera, **not wired yet** |

## Limitations (read before flashing)

1. **It relies on the firmware's "factory test mode".** We do not replace the firmware;
   we flip it into a mode the vendor ships anyway. A firmware update from the vendor
   may break it.
2. **The camera sweeps its motors and beeps at boot** (vendor behaviour in that mode).
3. **No DHCP in factory mode** — the camera needs a **static IP** written on the card.
4. **Presets are kept in RAM** — after a reboot the camera forgets `level`, so a Frigate
   `return_preset: level` lands on a stale position until re-saved.
5. **Tilt axis conventions** are not what you expect on this hardware: `y=0` is *not* the
   middle of the range, and the sign is inverted relative to some ONVIF clients. This
   project calibrates it against a saved `level` preset instead of assuming.
6. **Sub-stream is build-dependent** (encoder present, only one RTSP session exposed on
   some builds).
7. Not a Tuya-cloud replacement for *configuration*: cloud features (notifications,
   cloud recording) are gone by design.

## Safety & rollback

Nothing is irreversible as long as you keep the original `APP` partition: the installer
dumps it before touching anything, and `docs/en/rollback.md` explains how to restore a
stock camera. Worst case on these units is a reflash over telnet, which we have done
dozens of times. **Warranty: void.** Your call.

## Legal

MIT licensed (see `LICENSE`). We reverse-engineered the vendor firmware for
interoperability — a well-established right in most jurisdictions — and we **do not
redistribute vendor binaries**: the patchers are scripts that modify a dump of *your own*
camera. Third-party components and their licences are listed in `THIRD_PARTY.md`.

Not affiliated with Tuya, Anyka or LSC. Use at your own risk.

## Docs

| | |
|---|---|
| Install, card-free | [docs/en/install-pc.md](docs/en/install-pc.md) |
| Install, SD-only | [docs/en/install-sd.md](docs/en/install-sd.md) |
| AP provisioning | [docs/en/install-ap.md](docs/en/install-ap.md) |
| Requirements | [docs/en/requirements.md](docs/en/requirements.md) |
| Architecture | [docs/en/architecture.md](docs/en/architecture.md) |
| Frigate | [docs/en/frigate.md](docs/en/frigate.md) |
| Rollback | [docs/en/rollback.md](docs/en/rollback.md) |
| Limitations | [docs/en/limitations.md](docs/en/limitations.md) |
| Troubleshooting | [docs/en/troubleshooting.md](docs/en/troubleshooting.md) |
| FAQ | [docs/en/faq.md](docs/en/faq.md) |

## Contributing

Issues and pull requests welcome — especially **build reports** from other Anyka/Tuya
PTZ models (`share/VERSION`, `anyka_ipc` md5, whether `:554` appears). One camera model
at a time: open an issue, we add it to the compatibility table.
