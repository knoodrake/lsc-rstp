# Known limitations

Read this before flashing. Everything here is a deliberate compromise, not a bug we
forgot to fix.

1. **We rely on the vendor's factory test mode.**
   We do not replace the firmware: we make the camera boot in a mode the vendor ships
   anyway. A vendor firmware update can change or remove that mode.

2. **The camera sweeps its motors and beeps at boot.**
   Vendor behaviour of that mode. Expect the head to move on power-up, so mount the
   camera where a sweep cannot hit anything.

3. **No DHCP in factory mode.**
   The DHCP client exists in the firmware but is not started in this mode: the camera
   needs a **static IP** written on the card.

4. **Presets are persisted, and the camera returns to `level` at boot.**
   Presets are written to `/etc/config/lsc_presets.txt` (jffs2, a few dozen bytes)
   and restored at startup, so a Frigate `return_preset: level` keeps working after
   every power cycle. The daemon also drives the camera back to `level` at startup,
   which is what stops it from staring at the ceiling after a reboot.
   Save it once with `installer/verify.py <ip> --level x,y`.
   A camera whose tilt axis is mounted the other way round can be corrected per
   camera with `tilt_sign=-1` in `/etc/config/lsc_onvif.conf`.

5. **Tilt conventions are unusual.**
   `y = 0` is not the middle of the range and the sign is inverted relative to what
   some clients assume. This project calibrates against the `level` preset instead of
   hardcoding a convention (see [architecture](architecture.md)).

6. **Sub-stream is build-dependent.**
   Some builds expose `/videoSub` (640×360). Others only open one RTSP session; the
   encoder settings are there but the vendor program never serves the second stream.
   A patcher is provided in `patchers/` (experimental, applies to your own dump).

7. **The application partition is nearly full.**
   ~5 MiB in total, and we add ~600 KiB. The installer checks the size and retries with
   a larger squashfs block if needed.

8. **PTZ counter drift after a mechanical stop.**
   If the motor hits its physical end, the internal counter can drift. The `level`
   calibration above is the pragmatic answer; a full recalibration is on the roadmap.

9. **Cloud features are gone by design.** No notifications, no cloud recording, no
   vendor app.

10. **This is not a firmware replacement.** The vendor program still runs; that is the
    whole point (it is what produces the H.264 streams).
