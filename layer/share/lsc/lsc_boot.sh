#!/bin/sh
# LSC B3.3 (2026-11-04) - bootstrap card-free porté par l'APP (/usr/share/lsc).
# Idempotent : une seule instance (pid dans /tmp/lsc_boot.pid).
# 1) shadow root sans mot de passe (l'usine fait "rm /etc/config/shadow" à
#    chaque boot ; en mode usine elle le recrée depuis la SD seulement) ;
# 2) boucle 20 s : marqueur /tmp/_ak39_factory.ini (mode test usine => RTSP
#    prod :554) + respawn busybox telnetd :24 (comm=busybox => survit au
#    "killall -9 telnetd" d'anyka_ipc).
# Priorité du marqueur : /etc/config (par caméra) puis /usr/share/lsc (APP).
PIDF=/tmp/lsc_boot.pid
if [ -f "$PIDF" ]; then
  OLD=`cat "$PIDF" 2>/dev/null`
  if [ -n "$OLD" ] && kill -0 "$OLD" 2>/dev/null; then
    exit 0
  fi
fi
echo $$ > "$PIDF"

if [ ! -s /tmp/lsc_shadow ]; then
  printf 'root::0:0:99999:7:::\n' > /tmp/lsc_shadow
  mount --bind /tmp/lsc_shadow /etc/config/shadow 2>/dev/null
fi

while : ; do
  if [ ! -s /tmp/_ak39_factory.ini ]; then
    cp -f /etc/config/_ak39_factory.ini /tmp/_ak39_factory.ini 2>/dev/null
    if [ ! -s /tmp/_ak39_factory.ini ] && [ -s /usr/share/lsc/_ak39_factory.ini ]; then
      cp -f /usr/share/lsc/_ak39_factory.ini /tmp/_ak39_factory.ini 2>/dev/null
      IP=`ifconfig eth0 2>/dev/null | awk '/inet addr/ {sub("addr:","",$2); print $2; exit}'`
      [ -n "$IP" ] && sed -i "s/^ip=.*/ip=${IP}/" /tmp/_ak39_factory.ini 2>/dev/null
    fi
  fi
  netstat -tln 2>/dev/null | grep -q ':24 ' || busybox telnetd -l /bin/sh -p 24 >/dev/null 2>&1
  # 3) démon ONVIF embarqué (:8080, backend ptzd :8091) — sans lui Frigate n'a
  #    plus besoin du shim hôte. Relancé s'il a disparu ; journal borné à 64 K.
  if ! netstat -tln 2>/dev/null | grep -q ':8080 '; then
    setsid /usr/sbin/lsc_onvif --port 8080 >>/tmp/lsc_onvif.log 2>&1 &
  fi
  if [ -f /tmp/lsc_onvif.log ] && [ "`stat -c %s /tmp/lsc_onvif.log 2>/dev/null || echo 0`" -gt 65536 ]; then
    tail -n 200 /tmp/lsc_onvif.log > /tmp/lsc_onvif.log.rot 2>/dev/null
    cat /tmp/lsc_onvif.log.rot > /tmp/lsc_onvif.log 2>/dev/null
    rm -f /tmp/lsc_onvif.log.rot
  fi
  sleep 20
done
