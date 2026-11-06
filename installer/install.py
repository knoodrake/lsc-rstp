#!/usr/bin/env python3
"""LSC PTZ camera installer — card-free bake.

Dumps the camera's OWN application partition, injects only our layer into it,
rebuilds the squashfs image, then flashes it back over the camera's root shell
with a full md5 readback. Nothing of the vendor firmware is replaced: the image
is built from the camera itself, so it always matches its build.

Usage:
    install.py <CAM_IP>                 # dump, build, verify, flash
    install.py <CAM_IP> --dry-run       # stop before flashing
    install.py <CAM_IP> --build-only    # stop after building the image
    install.py <CAM_IP> --reboot        # reboot the camera once the md5 matches
    install.py <CAM_IP> --verify        # only run the post-install checks

Requirements: python3, squashfs-tools (mksquashfs/unsquashfs), telnet access to
the camera (root shell on port 24, opened by the probe SD card).
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LAYER_DIR = os.path.join(ROOT, "layer")
PY = sys.executable
PART_SIZE = 5242880            # mtd7 size on this platform

sys.path.insert(0, HERE)
import cam  # noqa: E402  (telnet helper shipped in this directory)


def fail(msg):
    print("\n!! %s" % msg)
    sys.exit(1)


def sh(cmd):
    if isinstance(cmd, str):
        return subprocess.call(cmd, shell=True)
    print("   $ " + " ".join(os.path.basename(c) if i == 0 else c
                             for i, c in enumerate(cmd)))
    return subprocess.call(cmd)


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


SHIPPED = [
    "sbin/lsc_onvif",
    "sbin/ptzd",
    "sbin/ptzd-supervise.sh",
    "share/lsc/lsc_boot.sh",
]
GENERATED = ["share/lsc/_ak39_factory.ini"]


def layer_files():
    """Our canonical layer, as paths relative to the camera root."""
    return SHIPPED + GENERATED


def check_layer():
    print("== [1/7] layer integrity ==")
    sums = {}
    sumfile = os.path.join(LAYER_DIR, "MD5SUMS")
    if os.path.exists(sumfile):
        for line in open(sumfile):
            line = line.strip()
            if line and not line.startswith("#"):
                h, p = line.split(None, 1)
                sums[p.strip()] = h
    ok = True
    for rel in SHIPPED:
        p = os.path.join(LAYER_DIR, rel)
        if not os.path.exists(p):
            fail("missing layer file: %s" % rel)
        got = md5(p)
        want = sums.get(rel)
        state = "OK" if (want is None or got == want) else "MISMATCH (want %s)" % want[:8]
        if want and got != want:
            ok = False
        print("   %-32s %s  %s" % (rel, got[:8], state))
    if not ok:
        fail("layer does not match layer/MD5SUMS")


def camera_marker(ip, port):
    """Read the camera's own factory marker so the baked fallback matches it."""
    out = ""
    for p in ([port, 23, 24] if port else [24, 23]):
        try:
            for _, txt in cam.run(["cat /etc/config/_ak39_factory.ini 2>/dev/null"],
                                  host=ip, port=p):
                out += txt
            break
        except OSError:
            continue
    keys = ("ssid", "passphrase", "ip", "netmask", "gateway")
    vals = {}
    for line in out.splitlines():
        line = line.strip()
        for k in keys:
            if line.startswith(k + "="):
                vals[k] = line.split("=", 1)[1]
    return vals


def write_marker(vals, ip):
    p = os.path.join(LAYER_DIR, "share", "lsc", "_ak39_factory.ini")
    os.makedirs(os.path.dirname(p), exist_ok=True)
    data = "[wifi_station]\n"
    data += "ssid=%s\n" % vals.get("ssid", "")
    data += "passphrase=%s\n" % vals.get("passphrase", "")
    data += "ip=%s\n" % vals.get("ip", ip)
    data += "netmask=%s\n" % vals.get("netmask", "255.255.255.0")
    data += "gateway=%s\n" % vals.get("gateway", "")
    with open(p, "w") as f:
        f.write(data)
    print("   marker written from the camera itself (ssid=%r ip=%s)"
          % (vals.get("ssid", ""), vals.get("ip", ip)))


def main():
    ap = argparse.ArgumentParser(description="LSC PTZ installer (card-free bake)")
    ap.add_argument("cam")
    ap.add_argument("--port", type=int, default=24, help="camera root shell port")
    ap.add_argument("--dry-run", action="store_true", help="stop before flashing")
    ap.add_argument("--build-only", action="store_true", help="stop after building")
    ap.add_argument("--reboot", action="store_true", help="reboot once the md5 matches")
    ap.add_argument("--verify", action="store_true", help="post-install checks only")
    ap.add_argument("--work", default=os.path.join(tempfile.gettempdir(), "lsc_install"))
    a = ap.parse_args()

    if a.verify:
        return sh([PY, os.path.join(HERE, "verify.py"), a.cam])

    work = os.path.join(a.work, a.cam.replace(".", "_"))
    os.makedirs(work, exist_ok=True)

    check_layer()

    print("== [2/7] reachability: %s ==" % a.cam)
    if sh("ping -c1 -W2 %s >/dev/null 2>&1" % a.cam) != 0:
        fail("camera unreachable — check the SD probe card and the LAN")

    print("== [3/7] dump the camera's own app partition ==")
    raw = os.path.join(work, "app_mtd7.bin")
    if os.path.exists(raw) and os.path.getsize(raw) == PART_SIZE:
        print("   cached dump reused: %s" % raw)
    else:
        if sh([PY, os.path.join(HERE, "dump_app.py"), "--cam", a.cam,
               "--telnet-port", str(a.port), "--out", raw]) != 0:
            fail("dump failed")
    if os.path.getsize(raw) != PART_SIZE:
        fail("unexpected dump size: %d bytes" % os.path.getsize(raw))

    tree = os.path.join(work, "tree")
    if not os.path.isdir(os.path.join(tree, "sbin")):
        if os.path.isdir(tree):
            shutil.rmtree(tree)
        print("   unsquashfs...")
        if sh("unsquashfs -q -d %s %s >/dev/null 2>&1" % (tree, raw)) != 0:
            fail("unsquashfs failed on the camera dump")
    verf = os.path.join(tree, "share", "VERSION")
    version = open(verf, encoding="utf-8", errors="replace").read().strip() \
        if os.path.exists(verf) else ""
    if not version:
        fail("cannot read share/VERSION from the dump")
    print("   camera build: %s (kept as-is)" % version)

    print("== [4/7] build: camera firmware + our layer ==")
    write_marker(camera_marker(a.cam, a.port), a.cam)
    out = os.path.join(work, "app_install.sqsh4")
    base_cmd = [PY, os.path.join(HERE, "build_app.py"), "--name", a.cam,
                "--base", tree, "--version", version, "--out", out]

    def build(block=None):
        cmd = list(base_cmd)
        if block:
            cmd += ["--block", block]
        return sh(cmd)

    # A stock application partition is close to full on some builds: if the
    # default 128 K squashfs block does not fit, retry with 256 K (less overhead).
    ok = build() == 0 and os.path.getsize(out) <= PART_SIZE
    if not ok:
        print("   does not fit with -b 131072, retrying with -b 262144")
        try:
            os.remove(out)
        except OSError:
            pass
        if build("262144") != 0:
            fail("build failed (block 262144)")
    size = os.path.getsize(out)
    print("   image: %s (%d bytes, %d free of %d)"
          % (out, size, PART_SIZE - size, PART_SIZE))
    if size > PART_SIZE:
        fail("image does not fit in mtd7")

    print("== [5/7] verify the produced image ==")
    tmp = tempfile.mkdtemp(prefix="lsc_verify_")
    try:
        if sh("unsquashfs -q -d %s %s >/dev/null 2>&1" % (tmp, out)) == 0:
            for rel in layer_files():
                p = os.path.join(tmp, rel)
                if not os.path.exists(p):
                    fail("missing in image: %s" % rel)
                print("   %-32s %s" % (rel, md5(p)[:8]))
            svc = os.path.join(tmp, "sbin", "service.sh")
            if os.path.exists(svc):
                hooked = "LSC" in open(svc, errors="replace").read()
                print("   %-32s %s" % ("sbin/service.sh hook",
                                       "OK" if hooked else "MISSING"))
                if not hooked:
                    fail("service.sh hook was not applied")
        else:
            print("   (unsquashfs unavailable: skipping image verification)")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if a.dry_run or a.build_only:
        print("\n== DRY RUN: nothing was written to the camera ==")
        print("image ready: %s" % out)
        return 0

    print("== [6/7] flash mtd7 (erase + write + full md5 readback) ==")
    if sh([PY, os.path.join(HERE, "flash_app.py"), "--file", out,
           "--cam", a.cam, "--port", str(a.port), "--apply",
           "--wait", "300"]) != 0:
        fail("flash failed or not confirmed — see /tmp/flash_app.log on the camera")

    if a.reboot:
        print("== [7/7] reboot ==")
        cam.run(["sync", "reboot -f"], host=a.cam, port=a.port)
        print("   wait ~60 s, then run: python3 installer/verify.py %s" % a.cam)
    else:
        print("== [7/7] reboot required for the new app partition to be used ==")
        print("   then: python3 installer/verify.py %s" % a.cam)
    return 0


if __name__ == "__main__":
    sys.exit(main())
