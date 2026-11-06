#!/bin/sh
# LSC PTZ — P5 : entre en mode point d'acces pour le provisioning WiFi.
#
# Prouve sur cam_3 le 2026-11-06 : hostapd 2.11 (nl80211) passe l'interface en
# Mode:Master, ESSID LSC-SETUP, 172.14.10.1, udhcpd sert 172.14.10.2-52, et
# `apweb` repond 200 sur le formulaire puis ecrit /etc/config/_ak39_factory.ini.
#
# Usage (root, sur la camera) :
#   sh /tmp/enter_ap.sh [timeout_s]      # defaut 900 s
#
# Ce script NE TOUCHE PAS au flash. Il coupe la station WiFi (donc le reseau
# habituel de la camera) ; en fin de delai il remet la station et redemarre,
# ce qui rend la camera a son etat normal.

TIMEOUT=${1:-900}
LOG_BASE=/mnt/logs
mkdir -p $LOG_BASE 2>/dev/null || LOG_BASE=/etc/config
LOG=$LOG_BASE/ap.log
say() { echo "$(date) $*" >> $LOG; }

IP_AP=172.14.10.1
SSID=${SSID:-LSC-SETUP}
MY_DIR=${MY_DIR:-/tmp}
CONF=$MY_DIR/hostapd.conf
INI=/etc/config/_ak39_factory.ini

say "== enter_ap start (timeout ${TIMEOUT}s) =="
say "hostapd: $(ls -l $MY_DIR/hostapd $MY_DIR/hostapd.bin 2>&1 | sed 's/  */ /g')"
say "apweb:   $(ls -l $MY_DIR/apweb 2>&1)"

# 1. configuration hostapd (point d'acces ouvert : c'est un mode de provisioning)
cat > $CONF <<EOF
interface=wlan0
driver=nl80211
ssid=$SSID
hw_mode=g
channel=6
auth_algs=1
wmm_enabled=1
ignore_broadcast_ssid=0
EOF
cp $CONF /etc/config/hostapd.conf 2>/dev/null
say "conf ecrite : ssid=$SSID"

# 2. couper la station, charger le driver en mode AP, demarrer hostapd + udhcpd
killall wpa_supplicant 2>/dev/null
sleep 2
: > $MY_DIR/hostapd.out
( sh /usr/sbin/wifi_driver_rtl8188.sh ap >> $MY_DIR/wifi_ap.out 2>&1 ) &
sleep 12
say "iwconfig: $(iwconfig wlan0 2>&1 | head -2 | sed 's/  */ /g')"
say "ifconfig: $(ifconfig wlan0 2>&1 | head -2 | sed 's/  */ /g')"
say "hostapd out: $(tail -2 $MY_DIR/hostapd.out 2>/dev/null | sed 's/  */ /g')"

# 3. serveur de provisioning (port 80 = celui qu'attend un telephone)
pkill apweb 2>/dev/null
( $MY_DIR/apweb 80 $INI >> $MY_DIR/apweb.out 2>&1 ) &
sleep 2
say "apweb: $(head -1 $MY_DIR/apweb.out 2>/dev/null)"

# 3bis. maintien : le firmware relance wpa_supplicant (anyka_ipc) et peut
# remettre l'IP de la station -> on tue le supplicant et on re-applique l'IP.
# `anyka_ipc` tient le gestionnaire reseau du firmware : sans le tuer, il relance
# wpa_supplicant et remet l'interface en mode station (constate a +40 s le
# 2026-11-06). En AP le RTSP ne sert a rien, on le coupe.
killall anyka_ipc 2>/dev/null && say "anyka_ipc coupe (gestionnaire reseau)"
( while :; do
      sleep 3
      killall wpa_supplicant 2>/dev/null
      killall udhcpc 2>/dev/null
      if ! iwconfig wlan0 2>/dev/null | grep -q 'Mode:Master'; then
          say "maintien : AP perdu -> relance hostapd"
          killall hostapd.bin 2>/dev/null
          ( $MY_DIR/hostapd $CONF >> $MY_DIR/hostapd.out 2>&1 & )
          sleep 2
      fi
      case "$(ifconfig wlan0 2>/dev/null | grep -o '172.14.10.1')" in
          '') ifconfig wlan0 $IP_AP 2>/dev/null ;;
      esac
  done ) &
KEEP_PID=$!
say "boucle de maintien AP demarree (pid $KEEP_PID)"
( sleep $((TIMEOUT + 90)); say "chien de garde : reboot"; reboot -f ) &
GUARD_PID=$!

# 4. ping local de l'interface AP : sanity check
ping -c 2 -W 2 $IP_AP >> $LOG 2>&1

say "== AP pret : http://$IP_AP/ (SSID $SSID) =="
say "attente de la configuration (max ${TIMEOUT}s)..."

# 5. attente : si l'INI change (apweb l'ecrit) ou timeout -> on rend la main
T0=$(cat /proc/uptime | cut -d' ' -f1)
MD5_0=$(md5sum $INI 2>/dev/null | cut -d' ' -f1)
while :; do
    sleep 10
    NOW=$(cat /proc/uptime | cut -d' ' -f1)
    ELAPSED=$(awk "BEGIN{printf \"%d\", $NOW - $T0}")
    MD5_N=$(md5sum $INI 2>/dev/null | cut -d' ' -f1)
    case $((ELAPSED % 20)) in
        0|1|2) say "[${ELAPSED}s] $(iwconfig wlan0 2>&1 | sed -n 2p | sed 's/  */ /g')" ;;
    esac
    if [ "$MD5_N" != "$MD5_0" ]; then
        say "INI modifie apres ${ELAPSED}s -> reboot dans 5s"
        kill $KEEP_PID $GUARD_PID 2>/dev/null
        cp $LOG /etc/config/lsc_ap_last.log 2>/dev/null
        sleep 5
        sync
        reboot -f
        exit 0
    fi
    if [ "$ELAPSED" -ge "$TIMEOUT" ]; then
        say "timeout ${ELAPSED}s -> retour en mode station"
        break
    fi
done

# 6. retour station : un simple reboot remet tout dans l'etat normal
say "== retour a l'etat normal par reboot =="
kill $KEEP_PID $GUARD_PID 2>/dev/null
pkill apweb 2>/dev/null
killall hostapd.bin hostapd udhcpd 2>/dev/null
cp $LOG /etc/config/lsc_ap_last.log 2>/dev/null   # trace lisible apres reboot
sync
reboot -f
exit 0
