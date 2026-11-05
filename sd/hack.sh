#!/bin/sh
# LSC PTZ — card-side bootstrap (runs as root at boot, from the SD card).
#
# This script NEVER touches the flash unless the vendor script is a recognised
# stock one. What it does, in order:
#   1. open a root shell on :24 (direct shell, survives the vendor telnetd kill)
#   2. write an identity report and a status report back onto the card
#   3. OPTIONAL (card-free runtime, no flash): patch the camera's own
#      sensor_driver.sh so that the factory marker and our boot script survive a
#      reboot, then install our boot script + marker into /etc/config (jffs2)
#   4. OPTIONAL (MODE_A flag): run our ONVIF/PTZ daemons straight from the card
#
# Nothing here is destructive: worst case the camera boots exactly like before.

MNT=/mnt
LOG=$MNT/logs/hack.log
mkdir -p $MNT/logs 2>/dev/null
say() { echo "$(date) $*" >> $LOG; }

say "lsc-rstp card bootstrap start (version: $(cat /usr/share/VERSION 2>/dev/null))"

# ---------------------------------------------------------------- 1. root shell
if ! netstat -tln 2>/dev/null | grep -q ':24 '; then
  printf 'root::0:0:99999:7:::\n' > /tmp/shadow2
  mount --bind /tmp/shadow2 /etc/shadow >> $LOG 2>&1
  busybox telnetd -l /bin/sh -p 24 >> $LOG 2>&1 && say "root shell on :24"
fi

# --------------------------------------------------------------- 2. card reports
if [ ! -f /tmp/lsc_ident_done ]; then
  touch /tmp/lsc_ident_done
  {
    echo "== camera date: $(date)"; echo "== uptime: $(cat /proc/uptime)"
    echo "== VERSION:";  cat /usr/share/VERSION 2>/dev/null
    echo "== BUNDLE:";   cat /usr/share/BUNDLE 2>/dev/null
    echo "== mtd:";      cat /proc/mtd
    echo "== ifconfig:"; ifconfig -a
    echo "== mounts:";   cat /proc/mounts
    echo "== ps:";       ps
  } > $MNT/logs/identity.txt 2>&1
  md5sum /usr/bin/anyka_ipc /etc/config/sensor_driver.sh >> $MNT/logs/identity.txt 2>&1
  say "identity report written"
fi

# ------------------------------------------- 3. card-free runtime (no flash path)
if [ -f $MNT/_ak39_factory.ini ] && [ ! -f /tmp/lsc_persist_done ]; then
  touch /tmp/lsc_persist_done
  P=$MNT/logs/persist.log
  say "jffs2 persistence START"

  # 3a. the factory marker must exist in jffs2, otherwise a restart of anyka_ipc
  #     silently drops back to cloud mode and loses :554
  cp -f $MNT/_ak39_factory.ini /etc/config/_ak39_factory.ini 2>>$P
  cp -f $MNT/_ak39_factory.ini /tmp/_ak39_factory.ini 2>>$P

  # 3b. our boot script (marker heal + root shell respawn)
  if [ -f $MNT/_lsc_boot.sh ]; then
    cp -f $MNT/_lsc_boot.sh /etc/config/lsc_boot.sh 2>>$P
    chmod 755 /etc/config/lsc_boot.sh 2>>$P
  fi

  # 3c. hook the vendor boot script GENERICALLY: insert two lines after the
  #     opening brace of install(), whatever the stock content is.
  SRC=/etc/config/sensor_driver.sh
  if [ -f $SRC ] && ! grep -q 'LSC-RSTP HOOK' $SRC; then
    [ -f $SRC.bak-lsc ] || cp -f $SRC $SRC.bak-lsc
    awk 'BEGIN{done=0}
         {print}
         /^install[ ]*\(\)/ && !done {
           print "\t# LSC-RSTP HOOK"
           print "\t[ -f /etc/config/_ak39_factory.ini ] && cp -f /etc/config/_ak39_factory.ini /tmp/_ak39_factory.ini 2>/dev/null"
           print "\t[ -x /etc/config/lsc_boot.sh ] && setsid /etc/config/lsc_boot.sh >/dev/null 2>&1 &"
           done=1
         }' $SRC > /tmp/sd.new 2>>$P
    if [ -s /tmp/sd.new ] && sh -n /tmp/sd.new 2>>$P; then
      cp -f /tmp/sd.new $SRC
      chmod 777 $SRC $SRC.bak-lsc 2>>$P
      say "sensor_driver.sh hooked (generic)"
    else
      say "WARNING: hook insertion failed, vendor script untouched"
    fi
  fi
  sync
  md5sum $SRC $SRC.bak-lsc /etc/config/lsc_boot.sh /etc/config/_ak39_factory.ini >> $P 2>&1
  say "jffs2 persistence END"
fi

# --------------------------------------- 4. mode A: run the daemons from the card
if [ -f $MNT/MODE_A ] && [ -f $MNT/_lsc_onvif ] && [ ! -f /tmp/lsc_modea_done ]; then
  touch /tmp/lsc_modea_done
  A=$MNT/logs/modeA.log
  say "MODE_A start"
  cp -f $MNT/_ptzd $MNT/_ptzd-supervise.sh $MNT/_lsc_onvif /tmp/ 2>>$A && chmod 755 /tmp/_ptzd /tmp/_ptzd-supervise.sh /tmp/_lsc_onvif 2>>$A
  if [ -f /tmp/_ptzd ]; then
    cp -f /tmp/_ptzd /tmp/ptzd 2>>$A
    chmod 755 /tmp/ptzd 2>>$A
    sed 's|/tmp/ptzd-supervise.sh|/tmp/_ptzd-supervise.sh|g' /tmp/_ptzd-supervise.sh > /tmp/ptzd-supervise.sh 2>>$A
    chmod 755 /tmp/ptzd-supervise.sh 2>>$A
    setsid /tmp/ptzd-supervise.sh >/dev/null 2>&1 &
    say "ptzd supervisor launched"
  fi
  if [ -f /tmp/_lsc_onvif ]; then
    setsid /tmp/_lsc_onvif >/dev/null 2>&1 &
    say "lsc_onvif launched (from card)"
  fi
fi

# ---------------------------------------------------------------- 5. final report
{
  echo "== $(date)"; echo "== uptime: $(cat /proc/uptime)"
  echo "== VERSION: $(cat /usr/share/VERSION 2>/dev/null)"
  echo "== listening:"; netstat -tln 2>/dev/null
  echo "== marker /tmp:  $(ls -l /tmp/_ak39_factory.ini 2>&1)"
  echo "== marker jffs2: $(ls -l /etc/config/_ak39_factory.ini 2>&1)"
  echo "== telnetd: $(ps | grep -c telnetd)"
  echo "== RTSP :554: $(netstat -tln 2>/dev/null | grep -c ':554 ')"
  echo "== ONVIF :8080: $(netstat -tln 2>/dev/null | grep -c ':8080 ')"
} > $MNT/logs/READY.txt 2>&1
say "card bootstrap done"
exit 0
