#!/usr/bin/env python3
"""Construit l'APP B3.3 d'une caméra LSC **depuis son propre APP** (dump mtd7).

Pourquoi depuis son propre APP : chaque build a ses `modules/*.ko`, son
`bin/anyka_ipc`, son `service.sh`, ses scripts `sbin/*` ; on ne remplace donc
que ce qui est nécessaire (patch de `service.sh` + 4 fichiers ajoutés).

Ce que le patch ajoute à `sbin/service.sh` (stock, quel que soit le build) :
  (a) repli du marqueur de mode test usine (`/tmp/_ak39_factory.ini`) depuis
      `/etc/config` puis depuis `/usr/share/lsc`, juste AVANT `anyka_ipc.sh start`
      ⇒ couvre aussi les relances ; l'`ip=` est re-dérivée d'eth0 ;
  (b) lancement détaché de `/usr/share/lsc/lsc_boot.sh` (shell root :24 + heal
      du marqueur, idempotent par `/tmp/lsc_boot.pid`) ;
  (c) telnet root :23 en shell direct (`busybox telnetd -l /bin/sh`, comm
      « busybox » ⇒ survit au `killall -9 telnetd` d'anyka_ipc) ;
  (d) `ptzd` + superviseur bakés dans l'APP (copie /tmp + lancement détaché),
      sauf `--no-ptzd`.

Usage :
  build_b33_cam.py --name cam2 --base /tmp/sq_cam2 --version 3.10.62.5 \
                   --out firmware/build/app_b33_cam2.sqsh4 [--no-ptzd] [--block N]
"""
import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "layer")               # our canonical layer (shipped in this repo)
PART_SIZE = 5242880
MARKER = "LSC B3.3 (2026-11-04)"

BLOCK_MARKER_AND_PTZD = """\t# --- LSC B3.3 (2026-11-04) : mode test usine SANS aucun support externe --
\t# Le dd du haut de service.sh lit /mnt (carte SD) ; sans carte il ne cree
\t# rien et anyka_ipc repart en mode 0 (Tuya, pas de :554). On restaure donc
\t# le marqueur depuis /etc/config (jffs2, priorite camera) puis depuis
\t# /usr/share/lsc (repli APP). Place ICI, juste avant le lancement, cela
\t# couvre AUSSI les relances (anyka_ipc.sh restart, restart_service).
\tif [ ! -s "/tmp/${FACTORY_MODE_FILE}" ]; then
\t\tcp -f "/etc/config/${FACTORY_MODE_FILE}" "/tmp/${FACTORY_MODE_FILE}" 2>/dev/null
\t\tif [ ! -s "/tmp/${FACTORY_MODE_FILE}" ] && [ -s "/usr/share/lsc/${FACTORY_MODE_FILE}" ]; then
\t\t\tcp -f "/usr/share/lsc/${FACTORY_MODE_FILE}" "/tmp/${FACTORY_MODE_FILE}" 2>/dev/null
\t\t\tLSC_IP=`ifconfig eth0 2>/dev/null | awk '/inet addr/ {sub("addr:","",$2); print $2; exit}'`
\t\t\t[ -n "$LSC_IP" ] && sed -i "s/^ip=.*/ip=${LSC_IP}/" "/tmp/${FACTORY_MODE_FILE}" 2>/dev/null
\t\tfi
\t\t[ -s "/tmp/${FACTORY_MODE_FILE}" ] && echo "LSC B3.3: marqueur mode usine restaure sans SD"
\tfi
\tsh /usr/sbin/anyka_ipc.sh start 
"""

BLOCK_PTZD = """\t# LSC B2 (2026-11-03) : ptzd bake, sans carte SD. Copie vers /tmp (le
\t# superviseur vit sur /tmp/ptzd, protocole identique a redeploy.py), puis
\t# lancement detache. Defensif : pas de double lancement si ptzd tourne deja.
\tif ! pidof ptzd > /dev/null 2>&1; then
\t\tcp -f /usr/sbin/ptzd /tmp/ptzd
\t\tchmod 755 /tmp/ptzd
\t\tcp -f /usr/sbin/ptzd-supervise.sh /tmp/ptzd-supervise.sh
\t\tchmod 755 /tmp/ptzd-supervise.sh
\t\trm -f /tmp/ptzd.hb
\t\t(setsid sh /tmp/ptzd-supervise.sh >>/tmp/ptzd.log 2>&1 &)
\tfi
"""

BLOCK_TOP = """# --- LSC B3 (2026-11-03) : telnet root :23 en shell DIRECT ------------------
# -l /bin/sh = shell direct sans login (l'usine fait "rm /etc/config/shadow" a
# chaque boot => le login busybox ne matche jamais). Prefixe "busybox" =>
# comm = "busybox" => epargne par le "killall -9 telnetd" d'anyka_ipc.
for p in $(pidof telnetd); do
    case "$(tr '\\0' ' ' < /proc/$p/cmdline 2>/dev/null)" in
        *-p\\ 24*) ;;
        *) kill "$p" 2>/dev/null ;;
    esac
done
busybox telnetd -l /bin/sh
# --- LSC B3.3 (2026-11-04) : shell root :24 + heal marqueur, portes par l'APP --
# DETACHE (setsid + &) : lsc_boot.sh boucle a l'infini, un appel synchrone
# bloquerait service.sh avant start_service. Idempotent (/tmp/lsc_boot.pid).
[ -x /usr/share/lsc/lsc_boot.sh ] && { setsid /usr/share/lsc/lsc_boot.sh >/dev/null 2>&1 & }
# --- fin LSC B3.3 -------------------------------------------------------------

"""

ANCHOR_START = "\tsh /usr/sbin/anyka_ipc.sh start "
ANCHOR_TOP = 'dd if="/mnt/${AP_MODE_FILE}"'


def fail(msg):
    raise SystemExit("KO: " + msg)


def patch_service(path, with_ptzd=True):
    s = open(path, encoding="utf-8", errors="surrogateescape").read()
    if MARKER in s:
        print("  service.sh : déjà patché (idempotent)")
    else:
        if s.count(ANCHOR_START) != 1:
            fail(f"anchor start_service x{s.count(ANCHOR_START)} dans {path}")
        if s.count(ANCHOR_TOP) != 1:
            fail(f"anchor top x{s.count(ANCHOR_TOP)} dans {path}")
        s = s.replace(ANCHOR_START,
                      BLOCK_MARKER_AND_PTZD + (BLOCK_PTZD if with_ptzd else ""))
        s = s.replace(ANCHOR_TOP, BLOCK_TOP + ANCHOR_TOP)
        open(path, "w", encoding="utf-8", errors="surrogateescape").write(s)
        print("  service.sh : patché (marqueur + ptzd + telnet/lsc_boot)")
    r = subprocess.run(["/bin/sh", "-n", path], capture_output=True, text=True)
    if r.returncode != 0:
        fail(f"sh -n {path} : {r.stderr.strip()}")
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", required=True)
    ap.add_argument("--base", required=True, help="APP décompressé (unsquashfs -d)")
    ap.add_argument("--version", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--block", default="131072")
    ap.add_argument("--no-ptzd", action="store_true")
    args = ap.parse_args()
    if not os.path.isdir(args.base):
        fail(f"base absente : {args.base}")
    work = os.path.abspath(args.out)[:-len(".sqsh4")] if args.out.endswith(".sqsh4") else args.out + ".tree"
    if os.path.exists(work):
        shutil.rmtree(work)
    print(f"[1/4] copie de {args.base} -> {work}")
    shutil.copytree(args.base, work, symlinks=True)

    print("[2/4] patch sbin/service.sh")
    patch_service(os.path.join(work, "sbin", "service.sh"), not args.no_ptzd)

    print("[3/4] ajout des fichiers LSC")
    os.makedirs(os.path.join(work, "share", "lsc"), exist_ok=True)
    for src, dst in [("share/lsc/lsc_boot.sh", "share/lsc/lsc_boot.sh"),
                     ("share/lsc/_ak39_factory.ini", "share/lsc/_ak39_factory.ini"),
                     ("sbin/ptzd", "sbin/ptzd"),
                     ("sbin/ptzd-supervise.sh", "sbin/ptzd-supervise.sh"),
                     ("sbin/lsc_onvif", "sbin/lsc_onvif")]:
        if args.no_ptzd and "ptzd" in dst:
            continue
        s2 = os.path.join(SRC, src)
        d2 = os.path.join(work, dst)
        shutil.copy2(s2, d2)
        os.chmod(d2, 0o755 if d2.endswith((".sh", "ptzd", "lsc_onvif")) else 0o644)
        print(f"  + {dst} ({os.path.getsize(d2)} o)")
    # le marqueur APP doit être lisible par tous, pas exécutable
    os.chmod(os.path.join(work, "share", "lsc", "_ak39_factory.ini"), 0o644)
    ver = os.path.join(work, "share", "VERSION")
    old = open(ver).read().strip() if os.path.exists(ver) else "?"
    open(ver, "w").write(args.version + "\n")
    print(f"  VERSION {old} -> {args.version} (BUNDLE inchangé : "
          f"{open(os.path.join(work, 'share', 'BUNDLE')).read().strip()})")
    for sh in ("share/lsc/lsc_boot.sh", "sbin/ptzd-supervise.sh"):
        r = subprocess.run(["/bin/sh", "-n", os.path.join(work, sh)],
                           capture_output=True, text=True)
        if r.returncode != 0:
            fail(f"sh -n {sh} : {r.stderr.strip()}")

    print(f"[4/4] mksquashfs (-comp xz -b {args.block} -all-root -noappend)")
    out = os.path.abspath(args.out)
    if os.path.exists(out):
        os.remove(out)
    r = subprocess.run(["mksquashfs", work, out, "-comp", "xz", "-b", args.block,
                        "-all-root", "-noappend"], capture_output=True, text=True)
    if r.returncode != 0:
        fail("mksquashfs :\n" + r.stdout[-800:] + r.stderr[-800:])
    size = os.path.getsize(out)
    md5 = subprocess.run(["md5sum", out], capture_output=True, text=True).stdout.split()[0]
    s = subprocess.run(["unsquashfs", "-s", out], capture_output=True, text=True).stdout
    fs = [ln for ln in s.splitlines() if "Filesystem size" in ln]
    print(f"  {out}")
    print(f"  {size} o, md5 {md5}, {fs[0].strip() if fs else '?'}")
    print(f"  place : {PART_SIZE - size} o libres sur {PART_SIZE} "
          f"({100.0 * size / PART_SIZE:.1f} % de la partition)")
    lst = subprocess.run(["unsquashfs", "-l", out], capture_output=True, text=True).stdout
    for want in ("share/lsc/lsc_boot.sh", "share/lsc/_ak39_factory.ini",
                 "sbin/service.sh", "sbin/lsc_onvif", "share/VERSION"):
        print(f"  {'OK ' if want in lst else 'KO '}{want}")
    if size > PART_SIZE:
        fail(f"{size} o ne tient pas dans la partition ({PART_SIZE} o)")
    print(f"VERDICT: OK {args.name} (md5 {md5})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
