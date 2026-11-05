#!/usr/bin/env python3
"""Copie /dev/mtd7 (APP, 5 Mio) d'une caméra LSC vers l'hôte, par telnet + nc.

Aucun flash, aucune écriture côté caméra : la caméra ouvre `nc -l -p PORT < /dev/mtd7`
(les octets partent sur la connexion :9999, pas dans le socket telnet — donc pas de
mangling IAC), puis recalcule le md5 de la même plage en local ; l'hôte compare.

Usage : dump_mtd7.py --out firmware/dump/cam2_mtd7_APP.bin [--cam 192.168.1.50] [--port 24]
"""
import argparse
import hashlib
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
CAM_HOLD = os.path.join(HERE, "cam_hold.py")
SIZE = 5242880
BS, COUNT = 4096, SIZE // 4096
NET_PORT = 9999  # overridden by --port


def detect_port(ip, prefer=None):
    for port in ([prefer] if prefer else []) + [24, 23]:
        try:
            socket.create_connection((ip, port), timeout=2).close()
            print(f"telnet : {ip}:{port} OPEN")
            time.sleep(1.2)   # the camera shell needs a moment between two sessions
            return port
        except OSError:
            continue
    raise SystemExit(f"aucun telnet ouvert sur {ip} (24/23)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cam", default=os.environ.get("CAM_HOST", "192.168.1.50"))
    ap.add_argument("--port", type=int, help="data port on the camera (default: automatic)")
    ap.add_argument("--telnet-port", type=int, help="camera root shell port (default: autodetect 24/23)")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    tport = detect_port(args.cam, args.telnet_port)
    net = args.port or (9900 + os.getpid() % 90)   # unique per run: a stale
                                                # listener would accept but stay silent
    if os.path.exists(args.out) and not os.environ.get("FORCE"):
        raise SystemExit(f"{args.out} existe déjà (FORCE=1 pour écraser)")

    env = dict(os.environ)
    env["CAM_HOST"] = args.cam
    env["CAM_PORT"] = str(tport)
    cmd = (f"md5sum /usr/share/VERSION /usr/share/BUNDLE; grep mtd7 /proc/mtd; "
           f"nc -l -p {net} < /dev/mtd7; dd if=/dev/mtd7 bs={BS} count={COUNT} "
           f"2>/dev/null | md5sum; echo DUMP_DONE")
    logf = args.out + ".camlog"
    log = open(logf, "w")
    p = subprocess.Popen([sys.executable, CAM_HOLD, cmd, "DUMP_DONE", "360"],
                         stdout=log, stderr=subprocess.STDOUT, env=env)
    # the camera starts its listener only after a few shell commands: retry until
    # it accepts, instead of assuming a fixed delay.
    time.sleep(5.0)
    h = hashlib.md5()
    n = 0
    t0 = time.time()
    s = None
    for _ in range(20):
        try:
            s = socket.create_connection((args.cam, net), timeout=5)
            break
        except OSError:
            time.sleep(0.5)
    if s is None:
        raise SystemExit("camera listener never accepted on port %d" % net)
    s.settimeout(8)
    # Read exactly SIZE bytes then close: busybox `nc -l` may not close the
    # connection on its own once the file is exhausted, so waiting for EOF can
    # deadlock the session (and the camera-side md5 never gets printed).
    # Tolerate short stalls: the SPI flash read can pause while the camera does
    # other work, so a socket timeout is not the end of the transfer.
    stall = 0
    with open(args.out + ".part", "wb") as fh:
        while n < SIZE:
            try:
                b = s.recv(min(65536, SIZE - n))
            except socket.timeout:
                stall += 1
                if stall > 12:
                    break
                continue
            if not b:
                break
            stall = 0
            fh.write(b)
            h.update(b)
            n += len(b)
            if n % (512 * 1024) == 0:
                print("   ... %d / %d bytes" % (n, SIZE))
    s.close()
    dt = time.time() - t0
    log.close()
    out, _ = p.communicate()
    out = (out or "").replace("\r", "\n")
    cam_md5 = None
    for ln in out.splitlines():
        parts = ln.strip().split()
        if len(parts) == 2 and len(parts[0]) == 32 and parts[1] == "-":
            cam_md5 = parts[0]
    print(f"reçu {n} o en {dt:.1f} s ({n / max(dt, .001) / 1024:.0f} Ko/s)")
    print("caméra :", out.strip()[-300:])
    host_md5 = h.hexdigest()
    print(f"md5 hôte={host_md5} caméra={cam_md5}")
    if n != SIZE:
        print(f"ATTENTION taille {n} != {SIZE}")
    if cam_md5 and cam_md5 != host_md5:
        raise SystemExit("md5 différent — dump corrompu")
    os.replace(args.out + ".part", args.out)
    print(f"OK -> {args.out} ({os.path.getsize(args.out)} o)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
