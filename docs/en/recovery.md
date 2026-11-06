# Recovery — the camera can fix itself from the SD card

The LSC PTZ firmware ships a **built-in recovery path** that runs when the
application partition cannot be mounted. It is the safety net that saved our
test camera after an interrupted flash, and it is the fastest way back to a
working camera:

```
power on
  └─ /etc/init.d/rc.local
       ├─ mount -t squashfs /dev/mtdblock7 /usr        (the APP partition)
       └─ if /usr/sbin/main.sh is missing:
            /etc/init.d/system_recover.sh &
                 ├─ mount -t vfat /dev/mmcblk0p1 /mnt   (the SD card)
                 ├─ tar -xf /mnt/update.tar -C /tmp
                 ├─ dd each file present in the tar to its partition
                 └─ reboot -f
```

`system_recover.sh` and `rc.local` live in the **rootfs** partition
(`mtd5`), which no LSC operation ever writes, so the recovery path stays
available even when the application partition is completely broken.

## What the recovery accepts

A **tar archive named `update.tar` at the root of a FAT32 SD card**. Only the
partitions whose files are present are touched:

| File inside the tar | Partition written |
|---|---|
| `usr.sqsh4` | `APP` (`mtd7`) — the LSC layer lives here |
| `root.sqsh4` | `ROOTFS` (`mtd5`) |
| `uImage` | `KERNEL` (`mtd4`) |
| `cloudOS.dtb` | `DTB` (`mtd3`) |
| `usr.jffs2` | `CONFIG` (`mtd6`) |
| `env.img` | `ENV` (`mtd1`) |

So a recovery that only restores the application is simply a tar containing
one file:

```bash
cp /path/to/app.sqsh4 /tmp/usr.sqsh4
tar --format=ustar -cf update.tar -C /tmp usr.sqsh4
cp update.tar /media/SDCARD/          # root of the card, FAT32
```

Add `mtd_debug` (the camera's own binary, `/bin/mtd_debug`) next to
`update.tar` as a belt-and-braces measure: `system_recover.sh` looks for it in
`/bin`, then `/mnt`, then `/tmp`.

Then insert the card and **power cycle** the camera. It erases and rewrites the
partition, then reboots by itself (~1–2 minutes). There is nothing to type and
no network access needed.

## How to know it ran

* The camera reboots **once** on its own a minute or two after power-on.
* After that reboot `ping <ip>` answers and `telnet <ip> 24` gives a root shell
  (if the restored image contains the LSC layer).

If nothing happens after ~3 minutes, the application partition was still
mountable (`/usr/sbin/main.sh` found) and the recovery was simply not needed —
that is harmless, the tar is ignored.

## Preparing a recovery SD card before you experiment

Before flashing anything, keep a copy of the camera's **original** application
partition:

```bash
python3 installer/dump_app.py --cam 192.168.1.50 --out backup_mtd7.bin
```

That file is your way back to factory: put it in a tar named `update.tar` at the
root of a card, as shown above. `installer/install.py` also caches the dump it
works from in `/tmp/lsc_install/<ip>/app_mtd7.bin`.

## Related

* [Rollback](rollback.md) — reverting a baked layer over telnet.
* [Install over SD](install-sd.md) — the SD probe card, a different mechanism.
