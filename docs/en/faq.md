# FAQ

**Does it work without internet?**
Yes. The camera never contacts Tuya again after installation.

**Do I keep the Tuya app / cloud features?**
No: notifications and cloud recording are gone by design. Everything local (RTSP,
ONVIF, detection in Frigate) keeps working.

**Is it reversible?**
Yes. The application partition is dumped before being rewritten, and the hooks in the
small writable partition are backed up. See [rollback](rollback.md).

**Does it void the warranty?**
Almost certainly. Opening nothing, but rewriting a firmware partition.

**Do I need to solder or open the camera?**
No. It is done entirely over the network once the card has booted the camera.

**Why do I have to give a static IP?**
In factory test mode the firmware never starts its DHCP client (verified). A static IP
is the only reliable option.

**Why is there a card at all?**
It is the only way in the first time: the vendor firmware reads it at boot. After the
card-free install it is no longer needed. The SD-only mode keeps it as the carrier of
the daemons.

**Will it work on my camera?**
If it is an Anyka AK3918 / AK3168 based Tuya PTZ camera, probably. The installer builds
the image from your own dump, so it adapts. Untested models are experimental: open an
issue with your `share/VERSION`, the md5 of `anyka_ipc` and whether `:554` appears.

**Is the PTZ usable by other software than Frigate?**
Yes: it is standard ONVIF PTZ on `:8080` (`admin` / `admin`), and it also accepts
continuous moves.

**Why is the sub-stream missing on my camera?**
See the compatibility table: some builds never serve the second RTSP session. The
encoder is configured and the hardware is capable, the vendor program just does not
open it. An experimental patcher is in `patchers/`.

**Why `y=0` is not the middle?**
Because the tilt axis only uses half of the motor range and `home` is the folded
position. See [architecture](architecture.md).

**Can I update the vendor firmware later?**
Yes, but do it with our layer removed, then reinstall: a vendor update may replace the
application partition and drop our files.

**How can I help?**
Run `installer/verify.py <ip>` on your camera and paste the output in an issue, with
your `share/VERSION`. Build reports are the most valuable contribution right now.
