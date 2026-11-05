# Rollback

Nothing here is irreversible. Pick the lightest step that fits your situation.

## 1. Undo the factory-mode hooks only (keep the camera stock otherwise)

The card-side script writes at most three things into `/etc/config` (jffs2):

| File | What it is |
|---|---|
| `_ak39_factory.ini` | the factory marker (WiFi + static IP) |
| `lsc_boot.sh` | our boot loop |
| `sensor_driver.sh` | the vendor script, **backed up as `sensor_driver.sh.bak-lsc`** |

From a root shell (`telnet <ip> 24`):

```sh
cp /etc/config/sensor_driver.sh.bak-lsc /etc/config/sensor_driver.sh
rm -f /etc/config/lsc_boot.sh /etc/config/_ak39_factory.ini
sync
reboot -f
```

The camera boots in Tuya mode again (cloud, no RTSP). Its firmware is untouched.

## 2. Return to the previous application partition

The installer dumps the camera's **own** partition before touching anything, in its
working directory (`/tmp/lsc_install/<ip>/app_mtd7.bin` by default). Flash it back:

```bash
python3 installer/flash_app.py --file /tmp/lsc_install/<ip>/app_mtd7.bin --cam <ip> --apply
```

Wait for `FLASH_OK` on the camera (`cat /tmp/FLASH_OK`), then reboot.

## 3. Restore a vendor update

If you kept the vendor `update.bin`, the official way still works: put it on a FAT32
card as the vendor expects, insert it, power on. That reflashes the whole firmware and
removes every trace of this project.

## 4. Bricked: nothing but telnet

If the camera boots but RTSP/ONVIF is broken, you still have the root shell on port 24
(both the vendor's `service.sh` hook and our boot loop open it). From there you can
reflash as in step 2.

If the camera does not boot at all (rare: we only rewrite the application partition),
the vendor recovery is the OTA card. On these units we have never needed more than a
reflash over telnet.
