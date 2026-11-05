#!/usr/bin/env python3
"""Écrit un squashfs APP (**mtd7**) sur une caméra LSC par telnet, sans carte SD.

Chemin emprunté (aucun outil exotique, tout existe sur la caméra) :
  1. préflight   : VERSION, BUNDLE, table mtd, espace /tmp, outils mtd_*, `nc`
  2. transfert   : `nc -l -p 9999 | tar x -C /tmp`  (usr.sqsh4.new + flash_app.sh)
                   puis md5 côté caméra comparé à l'hôte AVANT tout effacement
  3. flash       : script détaché (setsid) qui copie les outils sur /tmp (l'APP
                   va être effacée !), `mtd_debug erase /dev/mtd7`, `mtd_update`,
                   puis **relecture complète** de mtd7 et md5
  4. verdict     : /tmp/FLASH_OK (+ md5 attendu) ou /tmp/FLASH_FAIL ; **pas de
                   reboot automatique** — l'hôte décide après avoir vu le md5
  5. reboot      : `sync; reboot -f` (jamais sans -f : cf. SKILLS S11)

Le paquet OTA officiel (update.bin + carte SD) reste la voie nominale ; ceci est
la voie « sans carte » utilisée pour le pilote cam_3. Le squashfs doit être
celui produit par build_b33.py (payload APP seul).

Usage :
  remote_flash_app.py --file firmware/build/app_b33.sqsh4 [--apply] [--reboot]
  remote_flash_app.py --status                # relit l'état du flash en cours
  remote_flash_app.py --file X --only md5     # juste un md5 distant
  remote_flash_app.py --file X --only script  # pousse/verifie seulement le script de flash
  remote_flash_app.py --file X --only launch  # lance le script deja present sur la camera
"""
import argparse
import base64
import hashlib
import io
import os
import socket
import subprocess
import sys
import tarfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
CAM_HOLD = os.path.join(HERE, "cam_hold.py")
MTD = "/dev/mtd7"
ERASE_LEN = "0x500000"
CHUNK = 4096

FLASH_SCRIPT = r'''#!/bin/sh
# LSC remote_flash_app - script de flash mtd7 (detache). Aucune dependance sur
# /usr : l'APP est effacee pendant l'operation, tout doit venir de /tmp ou du
# rootfs (/bin/busybox).
L=/tmp/flash_app.log
exec >>$L 2>&1
echo "=== START $(date +%s) ==="
cp -f /usr/bin/mtd_update /tmp/mtd_update 2>/dev/null || cp -f /bin/mtd_update /tmp/mtd_update
cp -f /bin/mtd_debug /tmp/mtd_debug 2>/dev/null || cp -f /usr/bin/mtd_debug /tmp/mtd_debug
chmod 755 /tmp/mtd_update /tmp/mtd_debug 2>/dev/null
ls -l /tmp/mtd_update /tmp/mtd_debug
rm -f /tmp/FLASH_OK /tmp/FLASH_FAIL
echo "--- erase ${ERASE_LEN} ---"
/tmp/mtd_debug erase ${MTD} 0x0 ${ERASE_LEN}
sync
echo "--- write (rc=$?) ---"
/tmp/mtd_update /tmp/usr.sqsh4.new ${MTD}
echo "mtd_update rc=$?"
sync
echo "--- readback ---"
M=`dd if=${MTD} bs=${CHUNK} count=${COUNT} 2>/dev/null | md5sum | awk '{print $1}'`
echo "md5=$M attendu=${EXPECTED}"
if [ "$M" = "${EXPECTED}" ]; then
    echo "OK" > /tmp/FLASH_OK
else
    echo "$M" > /tmp/FLASH_FAIL
fi
echo "=== DONE $(date +%s) ==="
'''


def detect_port(ip, prefer=None):
    """Port telnet de la caméra : 23 (FW baké/Tuya) ou 24 (hack SD card-free)."""
    for port in ([prefer] if prefer else []) + [23, 24]:
        try:
            socket.create_connection((ip, port), timeout=2).close()
            os.environ["CAM_PORT"] = str(port)
            print(f"telnet : {ip}:{port} OPEN")
            return port
        except OSError:
            continue
    raise SystemExit(f"aucun telnet ouvert sur {ip} (23/24)")


def md5_bytes(b):
    return hashlib.md5(b).hexdigest()


def md5_file(path):
    h = hashlib.md5()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def cam(cmd, sentinel="__DONE__", timeout=60.0):
    """UNE commande telnet tenue jusqu'au sentinelle (firmware/cam_hold.py)."""
    env = dict(os.environ)
    p = subprocess.run([sys.executable, CAM_HOLD, cmd, sentinel, str(timeout)],
                       capture_output=True, text=True, env=env)
    out = p.stdout.replace("\r", "\n")
    return p.returncode, out


def cam_grep(cmd, sentinel, patterns, timeout=60.0):
    """Exécute et renvoie les lignes qui matchent un des préfixes attendus."""
    rc, out = cam(cmd, sentinel, timeout)
    lines = [ln.strip() for ln in out.splitlines()]
    picked = {}
    for ln in lines:
        for key in patterns:
            if ln.startswith(key):
                picked.setdefault(key, []).append(ln[len(key):].strip())
    return rc, picked, out


def preflight():
    cmd = ("cat /usr/share/VERSION; echo V=; cat /usr/share/BUNDLE; echo B=; "
           "grep mtd7 /proc/mtd; echo M=; df -k /tmp | tail -1; echo D=; "
           "ls -l /usr/bin/mtd_update /bin/mtd_debug; echo T=; "
           "ls /bin/nc /usr/bin/nc /bin/md5sum /bin/awk 2>&1; echo N=; "
           "ps | grep -c anyka; echo P=; uptime; echo PF_DONE")
    rc, out = cam(cmd, "PF_DONE", 30)
    print("-" * 70)
    print(out.strip()[-1500:])
    print("-" * 70)
    return rc == 0


def _send_file(cam_ip, port, path, chunk=32768, sleep=0.0):
    """Envoie le fichier en morceaux calibrés (pas de `sendall` monolithique)."""
    with open(path, "rb") as fh:
        data = fh.read()
    s = socket.create_connection((cam_ip, port), timeout=10)
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 32768)
    except OSError:
        pass
    t0 = time.time()
    mv = memoryview(data)
    try:
        for i in range(0, len(data), chunk):
            s.sendall(mv[i:i + chunk])
            if sleep:
                time.sleep(sleep)
    finally:
        try:
            s.shutdown(socket.SHUT_WR)
        except OSError:
            pass
        s.close()
    return len(data), time.time() - t0


def transfer(local_file, cam_ip, port=9999, timeout=300.0, chunk=32768, sleep=0.0):
    """`nc -l -p PORT > /tmp/usr.sqsh4.new` côté caméra (écriture directe, sans tar).

    Le `tar x -C /tmp` initial a été abandonné : il mourait ~800 Kio après le
    début du transfert (probable pression mémoire busybox), alors que l'écriture
    directe tient 5 Mio sans difficulté (37 Mo/s mesurés en dd).
    """
    env = dict(os.environ)
    cmd = (f"nc -l -p {port} > /tmp/usr.sqsh4.new; sync; "
           f"md5sum /tmp/usr.sqsh4.new; echo UP_DONE")
    p = subprocess.Popen([sys.executable, CAM_HOLD, cmd, "UP_DONE", str(timeout)],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, env=env)
    time.sleep(2.0)                      # laisser le listener s'ouvrir
    err, n, dt = "", 0, 0.0
    try:
        n, dt = _send_file(cam_ip, port, local_file, chunk, sleep)
    except Exception as e:               # noqa: BLE001 - on veut le diagnostic
        err = f"{type(e).__name__}: {e}"
    try:
        out, _ = p.communicate(timeout=timeout + 15)
    except subprocess.TimeoutExpired:
        p.kill()
        out, _ = p.communicate()
    out = (out or "").replace("\r", "\n")
    md5s = {}
    for ln in out.splitlines():
        parts = ln.strip().split()
        if len(parts) == 2 and len(parts[0]) == 32:
            md5s[os.path.basename(parts[1])] = parts[0]
    print(f"transfert : {n} o en {dt:.1f} s ({n / max(dt, 0.001) / 1e6:.2f} Mo/s)"
          f"{' — ' + err if err else ''}")
    print(f"md5 côté caméra : {md5s}")
    return md5s, out, err


def launch(count, expected):
    script = (FLASH_SCRIPT.replace("${ERASE_LEN}", ERASE_LEN)
              .replace("${MTD}", MTD).replace("${CHUNK}", str(CHUNK))
              .replace("${COUNT}", str(count)).replace("${EXPECTED}", expected))
    b64 = base64.b64encode(script.encode()).decode()
    chunks = [b64[i:i + 76] for i in range(0, len(b64), 76)]
    cmds = [": > /tmp/flash_app.b64"]
    cmds += [f"echo '{c}' >> /tmp/flash_app.b64" for c in chunks]
    cmds.append("base64 -d /tmp/flash_app.b64 > /tmp/flash_app.new; "
                "chmod 755 /tmp/flash_app.new; md5sum /tmp/flash_app.new; echo SC_DONE")
    got = None
    for i in range(0, len(cmds), 6):
        rc, out = cam("; ".join(cmds[i:i + 6]), "SC_DONE", 60)
        for ln in out.splitlines():
            parts = ln.strip().split()
            if len(parts) == 2 and len(parts[0]) == 32 and parts[1].endswith("flash_app.new"):
                got = parts[0]
    return script, got


def status():
    cmd = ("echo LOG=; cat /tmp/flash_app.log 2>/dev/null | tail -25; "
           "echo OK=; ls -l /tmp/FLASH_OK /tmp/FLASH_FAIL 2>/dev/null; "
           "cat /tmp/FLASH_OK /tmp/FLASH_FAIL 2>/dev/null; echo ST_DONE")
    rc, out = cam(cmd, "ST_DONE", 30)
    print(out.strip()[-2000:])
    ok = "FLASH_OK" in out
    bad = "FLASH_FAIL" in out
    return 0 if ok else (1 if bad else 2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", help="squashfs APP à écrire (build_b33.py)")
    ap.add_argument("--cam", default=os.environ.get("CAM_HOST", "192.168.1.50"))
    ap.add_argument("--port", type=int, help="port telnet (défaut : auto 23 puis 24)")
    ap.add_argument("--apply", action="store_true", help="sinon dry-run (préflight seul)")
    ap.add_argument("--reboot", action="store_true", help="reboot -f après FLASH_OK")
    ap.add_argument("--status", action="store_true")
    ap.add_argument("--only", choices=["md5", "script", "launch"],
                    help="md5 distant | pousser le script | lancer le script present")
    args = ap.parse_args()
    os.environ["CAM_HOST"] = args.cam
    detect_port(args.cam, args.port)

    if args.status:
        return status()

    if args.only == "md5":
        cmd = (f"dd if={MTD} bs={CHUNK} count=1204 2>/dev/null | md5sum; echo ONLY_DONE")
        rc, out = cam(cmd, "ONLY_DONE", 240)
        print(out.strip()[-500:])
        return 0

    if args.only in ("script", "launch"):
        if not args.file or not os.path.isfile(args.file):
            raise SystemExit("--file requis")
        sq_md5 = md5_file(args.file)
        size = os.path.getsize(args.file)
        if size % CHUNK:
            raise SystemExit(f"{size} o n'est pas multiple de {CHUNK}")
        count = size // CHUNK
        if args.only == "script":
            script, got = launch(count, sq_md5)
            want = md5_bytes(script.encode())
            print(f"script : {len(script)} o caméra={got} hôte={want}")
            return 0 if got == want else 1
        cmd = ("mv -f /tmp/flash_app.new /tmp/flash_app.sh; "
               "setsid sh /tmp/flash_app.sh >/dev/null 2>&1 & echo LAUNCHED")
        rc, out = cam(cmd, "LAUNCHED", 30)
        print("flash lancé :", "LAUNCHED" in out)
        return 0 if "LAUNCHED" in out else 1

    if not args.file or not os.path.isfile(args.file):
        raise SystemExit("--file requis")
    sq = args.file
    sq_md5 = md5_file(sq)
    size = os.path.getsize(sq)
    count = size // CHUNK
    if size % CHUNK:
        raise SystemExit(f"{size} o n'est pas multiple de {CHUNK}")
    print(f"squashfs : {sq} ({size} o, md5 {sq_md5}, count={count})")
    if not args.apply:
        preflight()
        print("dry-run : rien n'a été écrit (--apply pour flasher)")
        return 0

    if not preflight():
        raise SystemExit("préflight KO")
    script, script_md5 = launch(count, sq_md5)    # /tmp/flash_app.new sur la caméra
    want = md5_bytes(script.encode())
    print(f"script de flash : {len(script)} o, md5 caméra={script_md5} hôte={want}")
    if script_md5 and script_md5 != want:
        raise SystemExit("script de flash corrompu (base64/telnet) — rien n'a été effacé")
    md5s = {}
    # (chunk, sleep) : les gros blocs saturent le WiFi de cam_3 (TCP trickle) ;
    # 1024/5 ms est la cadence mesurée fiable >> rapide à la fin de l'échelle.
    for attempt, (chunk, sleep) in enumerate(
            [(4096, 0.002), (1024, 0.005), (8192, 0.02), (32768, 0.0)], 1):
        print(f"-- transfert tentative {attempt} (chunk={chunk}, sleep={sleep})")
        md5s, out, err = transfer(sq, args.cam, chunk=chunk, sleep=sleep)
        if md5s.get("usr.sqsh4.new") == sq_md5:
            break
        print(out.strip()[-600:])
    if md5s.get("usr.sqsh4.new") != sq_md5:
        raise SystemExit(f"transfert corrompu : {md5s} != {sq_md5} — rien n'a été effacé")
    print("transfert vérifié (md5 caméra == hôte)")
    cmd = ("mv -f /tmp/flash_app.new /tmp/flash_app.sh; "
           "setsid sh /tmp/flash_app.sh >/dev/null 2>&1 & echo LAUNCHED")
    rc, out = cam(cmd, "LAUNCHED", 30)
    print("flash lancé :", "LAUNCHED" in out)
    print("suivre : remote_flash_app.py --status")
    return 0


if __name__ == "__main__":
    sys.exit(main())
