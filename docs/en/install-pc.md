# Install — card-free mode (PC)

This is the recommended mode: after it, the camera runs our layer from its own
firmware partition, **no SD card needed**, and everything survives a reboot.

Total time: about 10 minutes, most of it waiting for the camera to boot or reboot.

## 0. Prepare

```bash
git clone https://github.com/LocalActionTuyaCams/lsc-rstp.git
cd lsc-rstp
```

You need `squashfs-tools` and Python 3.9+ (see [requirements](requirements.md)).

## 1. Write the probe card

```bash
sudo python3 installer/sd_card.py \
    --ssid "YourWiFi" --pass "YourWiFiPassword" \
    --ip 192.168.1.50 --netmask 255.255.255.0 --gateway 192.168.1.1 \
    --out /media/$USER/CARD
```

Choose an IP **outside your DHCP pool** (or reserve it on your router). The camera
will use exactly this address.

## 2. Boot the camera with the card

Insert the card, power on, wait ~60 s. The camera enters its factory test mode: the
motors sweep (vendor behaviour), RTSP comes up, and a root shell opens on port 24.

Verify the entry point, then **remove the card** (it is no longer needed):

```bash
telnet 192.168.1.50 24
# you should get a root shell prompt
curl http://192.168.1.50:554/ 2>/dev/null   # or: ffprobe rtsp://192.168.1.50:554/main_ch
```

If you prefer to read the camera's own report, put the card back in your PC and read
`logs/READY.txt`.

## 3. Bake the camera

```bash
python3 installer/install.py 192.168.1.50 --dry-run   # optional: stop before writing
python3 installer/install.py 192.168.1.50 --reboot    # dump, build, flash, reboot
```

What it does, with checks at every step:

1. verifies our layer against `layer/MD5SUMS`;
2. reads your camera's own factory marker so the baked fallback matches your WiFi;
3. **dumps the camera's own app partition** (`/dev/mtd7`) over telnet — this is your
   rollback image, keep the working directory;
4. injects only our files into *that* dump, patches `service.sh` with a generic hook;
5. rebuilds the squashfs and verifies our files inside the produced image;
6. flashes with `mtd_debug erase` + `mtd_update` + **full md5 readback**;
7. reboots (only if you asked).

The camera reboots into the new partition. This takes about a minute.

## 4. Verify

```bash
python3 installer/verify.py 192.168.1.50
```

You want `VERDICT: READY`. It checks RTSP, ONVIF, the PTZ backend, the ONVIF
conformance suite and the presets, and prints the Frigate configuration snippet.

## 5. Choose the standby position

The only thing that cannot be automated is where the camera should look when idle.
Aim it by hand (or with `--level`), then save the preset:

```bash
python3 installer/verify.py 192.168.1.50 --level 0.30,0.00
```

This moves the camera in closed loop and saves the `level` preset, which is also what
`return_preset: level` uses in Frigate.

Done. See [frigate.md](frigate.md) to plug it into your NVR.
