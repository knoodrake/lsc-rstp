#!/bin/sh
# LSC PTZ — P5 : bootstrap cote carte SD (execute par le firmware au boot, root).
#
# Contenu attendu de la carte (FAT32, racine) :
#   hack.sh        <- ce fichier
#   enter_ap.sh    <- mise en mode point d'acces + page de provisioning
#   hostapd        <- enveloppe (compat -B) posee en /tmp/hostapd
#   hostapd.bin    <- vrai hostapd 2.11 ARMv5
#   apweb          <- mini serveur HTTP de provisioning
#
# Deroulement : on copie tout dans /tmp (RAM), on attend que le WiFi soit la,
# puis on lance enter_ap.sh en tache de fond. La camera passe en AP (SSID
# LSC-SETUP, http://172.14.10.1/) ; la page web ecrit l'INI WiFi et reboots.
#
# Rien n'est ecrit dans le flash. Retirer la carte = plus de mode AP au boot.

MNT=/mnt
LOG_DIR=$MNT/logs
mkdir -p $LOG_DIR 2>/dev/null || LOG_DIR=/tmp
LOG=$LOG_DIR/ap.log
say() { echo "$(date) $*" >> $LOG; }

say "== P5 card bootstrap =="

# 1. shell root :24 (depannage : c'est le seul acces tant qu'on est en AP)
if ! netstat -tln 2>/dev/null | grep -q ':24 '; then
    busybox telnetd -l /bin/sh -p 24 >> $LOG 2>&1 && say "shell root sur :24"
fi

# 2. copie en RAM de tout le kit (l'AP coupe le WiFi, pas la carte)
for f in enter_ap.sh hostapd hostapd.bin apweb; do
    if [ -f "$MNT/$f" ]; then
        cp "$MNT/$f" /tmp/$f >> $LOG 2>&1
        chmod 755 /tmp/$f
    else
        say "!! $f absent de la carte"
    fi
done

[ -x /tmp/enter_ap.sh ] || { say "kit incomplet, abandon"; exit 1; }

# 3. attendre que le driver WiFi ait cree wlan0 (le boot LSC prend ~40-60 s)
i=0
while [ ! -d /sys/class/net/wlan0 ] && [ $i -lt 40 ]; do
    sleep 2
    i=$((i+1))
done
sleep 5
say "wlan0 present apres $((i*2+5))s"

# 4. mode AP + page de provisioning
say "lancement de enter_ap.sh"
setsid sh /tmp/enter_ap.sh 900 >> $LOG 2>&1 &
say "== bootstrap termine =="
