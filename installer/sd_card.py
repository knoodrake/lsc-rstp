#!/usr/bin/env python3
"""Build the LSC PTZ probe card (the first contact with the camera).

The card is what makes a stock camera usable: on boot the vendor firmware reads
it, enters its factory test mode (RTSP + root shell) and runs our bootstrap
script. The card is also where the camera reports back (logs/READY.txt).

Usage:
    # card-free install path (recommended): card only, no extras
    sudo python3 installer/sd_card.py --ssid "YourWiFi" --pass "secret" \
        --ip 192.168.1.50 --netmask 255.255.255.0 --gateway 192.168.1.1 \
        --out /media/user/CARD

    # SD-only path (no PC at all, the card stays in the camera)
    sudo python3 installer/sd_card.py ... --mode-a --out /media/user/CARD

Then: insert the card, power the camera, wait ~60 s.
    telnet <ip> 24          -> root shell
    cat /mnt/logs/READY.txt -> what the camera did
"""
import argparse
import ipaddress
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SD = os.path.join(ROOT, "sd")
LAYER = os.path.join(ROOT, "layer")


def die(msg):
    print("!! " + msg)
    sys.exit(1)


def copy(src, dst, mode=0o644):
    shutil.copyfile(src, dst)
    os.chmod(dst, mode)


def main():
    ap = argparse.ArgumentParser(description="build the LSC PTZ probe card")
    ap.add_argument("--out", required=True, help="mounted FAT32 card mount point")
    ap.add_argument("--ssid", default="")
    ap.add_argument("--pass", dest="passphrase", default="")
    ap.add_argument("--ip", required=True, help="static IP given to the camera")
    ap.add_argument("--netmask", default="255.255.255.0")
    ap.add_argument("--gateway", default="")
    ap.add_argument("--mode-a", action="store_true",
                    help="also copy the daemons so the card can run the camera alone")
    ap.add_argument("--force", action="store_true", help="overwrite existing files")
    a = ap.parse_args()

    out = a.out
    if not os.path.isdir(out):
        die("not a directory: %s" % out)
    if not os.access(out, os.W_OK):
        die("not writable: %s (try sudo)" % out)
    try:
        ipaddress.IPv4Address(a.ip)
        ipaddress.IPv4Address(a.netmask)
        if a.gateway:
            ipaddress.IPv4Address(a.gateway)
    except Exception as e:
        die("invalid IP: %s" % e)
    if a.ssid and not (8 <= len(a.passphrase) <= 63):
        die("WiFi passphrase must be 8..63 characters (got %d)" % len(a.passphrase))

    plan = ["_ak39_factory.ini", "hack.sh", "hostapd", "_lsc_boot.sh", "shadow", "TZ"]
    if a.mode_a:
        plan += ["_lsc_onvif", "_ptzd", "_ptzd-supervise.sh", "MODE_A"]
    existing = [p for p in plan if os.path.exists(os.path.join(out, p))]
    if existing and not a.force:
        die("files already on the card (%s) — use --force" % ", ".join(existing[:4]))

    # 1. factory marker: WiFi credentials + static network configuration
    marker = "[wifi_station]\n"
    marker += "ssid=%s\n" % a.ssid
    marker += "passphrase=%s\n" % a.passphrase
    marker += "ip=%s\nnetmask=%s\ngateway=%s\n" % (a.ip, a.netmask, a.gateway)
    with open(os.path.join(out, "_ak39_factory.ini"), "w") as f:
        f.write(marker)

    # 2. bootstrap script + AP-mode wrapper
    copy(os.path.join(SD, "hack.sh"), os.path.join(out, "hack.sh"), 0o755)
    copy(os.path.join(SD, "hostapd"), os.path.join(out, "hostapd"), 0o755)

    # 3. our boot script (heals the marker, respawns the root shell)
    copy(os.path.join(LAYER, "share/lsc/lsc_boot.sh"),
         os.path.join(out, "_lsc_boot.sh"), 0o755)

    # 4. stock-camera plumbing
    copy(os.path.join(SD, "shadow"), os.path.join(out, "shadow"))
    copy(os.path.join(SD, "TZ"), os.path.join(out, "TZ"))

    # 5. optional: SD-only mode (card stays inserted, no PC needed afterwards)
    if a.mode_a:
        copy(os.path.join(LAYER, "sbin/lsc_onvif"), os.path.join(out, "_lsc_onvif"), 0o755)
        copy(os.path.join(LAYER, "sbin/ptzd"), os.path.join(out, "_ptzd"), 0o755)
        copy(os.path.join(LAYER, "sbin/ptzd-supervise.sh"),
             os.path.join(out, "_ptzd-supervise.sh"), 0o755)
        open(os.path.join(out, "MODE_A"), "w").write("SD-only mode\n")

    # 6. where the camera writes its reports
    os.makedirs(os.path.join(out, "logs"), exist_ok=True)
    with open(os.path.join(out, "README-FIRST.txt"), "w") as f:
        f.write("""LSC PTZ probe card
==================

1. Eject the card properly, insert it in the camera (slot under the head).
2. Power the camera on and wait about 60 seconds.
3. Read the reports the camera writes back onto this card:
     logs/READY.txt    what is listening now
     logs/identity.txt firmware build, md5 of the vendor binaries
     logs/hack.log     what the bootstrap did (and any warning)
4. Entry point:  telnet <camera-ip> 24   (root shell, no password)

Next step depends on the install mode you picked:
  - card-free (default): run installer/install.py <camera-ip> from a PC
  - SD-only (--mode-a): nothing else to do, the card must stay in the camera
""")

    total = sum(os.path.getsize(os.path.join(out, p)) for p in plan if os.path.exists(os.path.join(out, p)))
    print("card ready: %s" % out)
    print("  %d files, %.1f KiB" % (len(plan), total / 1024.0))
    print("  camera address: %s/%s%s" % (a.ip, a.netmask,
                                         " gw " + a.gateway if a.gateway else ""))
    print("  wifi: %s" % (a.ssid or "(none: keep the camera's current network)"))
    print("  mode: %s" % ("SD-only (card stays in)" if a.mode_a else "card-free (PC bake next)"))
    print("\nnext: insert the card, power on, wait 60 s, then")
    print("      telnet %s 24   (or cat the card's logs/READY.txt from your PC)" % a.ip)
    return 0


if __name__ == "__main__":
    sys.exit(main())
