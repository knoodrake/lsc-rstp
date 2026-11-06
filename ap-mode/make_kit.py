#!/usr/bin/env python3
"""Fabrique une carte SD de provisioning WiFi (P5) pour les cameras LSC PTZ.

Contenu de la carte produite (FAT32, racine) :
    hack.sh       bootstrap execute par le firmware au boot
    enter_ap.sh   mode point d'acces + page de provisioning + auto-retour
    hostapd       enveloppe compat (le script usine passe -B, hostapd 2.11 l'a perdu)
    hostapd.bin   hostapd 2.11 ARMv5 (kit/p5_hostapd/out/hostapd)
    apweb         serveur HTTP de provisioning (kit/p5_hostapd/out/apweb)
    README.txt    mode d'emploi cote utilisateur

Usage :
    python3 make_kit.py /media/<carte>          # ecrit dans un dossier existant
    python3 make_kit.py --check                 # verifie seulement les binaires

Prerequis : FAT32 (le firmware ne monte que du FAT), binaires deja construits
(`bash build.sh` et `bash build_apweb.sh`).
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FILES = {
    "hack.sh": os.path.join(HERE, "sd", "hack.sh"),
    "enter_ap.sh": os.path.join(HERE, "enter_ap.sh"),
    "hostapd": os.path.join(HERE, "launcher", "hostapd"),
    "hostapd.bin": os.path.join(HERE, "out", "hostapd"),
    "apweb": os.path.join(HERE, "out", "apweb"),
}

README = """LSC PTZ - carte de provisioning WiFi (P5)
==========================================

1. Formater une carte SD en FAT32 (MBR, pas d'exFAT).
2. Copier TOUS les fichiers de ce dossier a la racine de la carte.
3. Couper la camera, inserer la carte, remettre sous tension.
4. Apres ~1 minute, la camera emet le reseau WiFi :

       SSID : LSC-SETUP      (ouvert, pas de mot de passe)
       Page : http://172.14.10.1/

   (le WiFi de la maison est coupe pendant l'operation : c'est normal)

5. Se connecter au reseau LSC-SETUP avec un telephone ou un PC, ouvrir
   http://172.14.10.1/ et saisir : SSID du WiFi + mot de passe (+ IP fixe
   si le reseau l'exige).
6. Enregistrer : la camera ecrit sa configuration, redemarre et rejoint
   le WiFi saisi. Retirer la carte SD.

Sans configuration au bout de 15 minutes, la camera redemarre et repart
comme avant (carte SD inchangee, aucun risque pour le firmware).

Journal : /mnt/logs/ap.log sur la carte (si la carte reste montee) et
/tmp/ap.log sur la camera. Depannage : telnet <ip>:24 (shell root direct).

Rien n'est ecrit dans le flash : la carte est le seul support modifie.
"""


def check() -> int:
    ok = True
    for name, path in FILES.items():
        if not os.path.isfile(path):
            print(f"MANQUANT  {name:12s} <- {path}")
            ok = False
            continue
        print(f"ok        {name:12s} {os.path.getsize(path):>9d} o  {path}")
    return 0 if ok else 1


def main() -> int:
    args = [a for a in sys.argv[1:] if a != "--check"]
    if "--check" in sys.argv:
        return check()
    if len(args) != 1:
        print(__doc__)
        return 2
    dest = os.path.abspath(args[0])
    if not os.path.isdir(dest):
        print(f"KO: {dest} n'est pas un dossier existant")
        return 2
    if check() != 0:
        print("KO: binaires manquants (lance build.sh puis build_apweb.sh)")
        return 1

    for name, src in FILES.items():
        dst = os.path.join(dest, name)
        if os.path.exists(dst):
            print(f"remplace  {name}")
        shutil.copyfile(src, dst)
        os.chmod(dst, 0o755)

    with open(os.path.join(dest, "README.txt"), "w", encoding="utf-8") as f:
        f.write(README)
    # le firmware copie /mnt/shadow vers /etc/config en mode usine : utile au shell :24
    with open(os.path.join(dest, "shadow"), "w", encoding="utf-8") as f:
        f.write("root::0:0:99999:7:::\n")

    print(f"\nCarte prete dans {dest}")
    print(" 1) ejecter proprement, 2) carte dans la camera, 3) mise sous tension")
    print(" 4) se connecter au WiFi LSC-SETUP, 5) http://172.14.10.1/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
