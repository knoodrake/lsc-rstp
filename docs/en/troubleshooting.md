# Troubleshooting

## The camera does not answer after booting with the card

* Check the card is **FAT32** and that the files are at the card's **root** (not in a
  subdirectory).
* Put the card back in your PC and read `logs/READY.txt` and `logs/hack.log`: the
  camera writes there what it managed to do. No `logs/` at all means the firmware did
  not run the card script.
* Try the other telnet port: some builds keep the vendor shell on `:23`, ours is `:24`.

## `telnet <ip> 24` connects but nothing appears

The vendor `telnetd` may hold the port. Close the session, wait a few seconds and retry;
at most one connection per second is safe on these units.

## RTSP `:554` is closed

`/tmp/_ak39_factory.ini` was missing when the vendor program started. From the root
shell:

```sh
ls -l /tmp/_ak39_factory.ini /etc/config/_ak39_factory.ini
cp -f /etc/config/_ak39_factory.ini /tmp/   # then restart the stream service
/usr/sbin/anyka_ipc.sh restart
```

This is exactly what our boot loop automates once the layer is baked.

## `healthz` answers 503

The ONVIF daemon is up but the PTZ backend does not answer. `ptzd` lives in `/tmp` and
is restarted by its supervisor; check:

```sh
ps | grep -c ptzd          # on the camera
curl http://<ip>:8080/healthz
```

If `ptzd` is missing, restart it from the baked copy (card-free mode) or reboot with the
card inserted (SD-only mode).

## The camera moves the wrong way (up instead of down)

Expected on this hardware if the `level` preset has never been recorded: the tilt
calibration is anchored on it. Record your standby view:

```sh
python3 installer/verify.py <ip> --level 0.30,0.00
```

Then make sure Frigate uses `return_preset: level`.

## Autotracking "runs away" to the ceiling

Two historical causes, both fixed in the shipped daemon: an inverted tilt sign, and a
zero point sitting at the ceiling. If you built the daemon yourself, check that
`TILT_SIGN` is `+1.0` and that the calibration path above is used.

## Frigate log: "is already performing an action"

Frigate serialises PTZ moves and occasionally keeps its internal lock. Updating to the
Frigate version used for testing (0.18 series) and restarting the container clears it.
If it persists, the ONVIF daemon is answering too slowly — check `healthz`.

## Frigate log: 454 Session Not Found on `/videoSub`

That build exposes only one RTSP session. Detect on `/main_ch` instead.

## `unsquashfs` or `mksquashfs` missing

Install `squashfs-tools`. The installer stops with a clear message before writing
anything.

## The flash failed halfway

Nothing is lost: the flash script erases and rewrites the partition, and it verifies the
resulting md5. Read the camera log:

```sh
cat /tmp/flash_app.log
ls -l /tmp/FLASH_OK /tmp/FLASH_FAIL
```

Then simply run the installer again — it reuses the dump it already made.
