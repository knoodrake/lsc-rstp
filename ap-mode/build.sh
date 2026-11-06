#!/bin/bash
# P5 — cross-compilation de hostapd 2.11 (ARMv5 / uClibc 1.0.28 de la caméra).
# ÉTAT 2026-11-05 01:1x : ✔ binaire produit ET exécuté sur cam_3 (`/tmp/hostapd -v`),
#   out/hostapd  727 336 o  md5 cceb717b80e0d70f88b14cdab00e1055
#
# Pourquoi hostapd standard (nl80211) suffit : le module WiFi du firmware
# `rtl8188ftv.ko` est un driver **mac80211** (`modinfo`: depends: usbcore,cfg80211 ;
# 215 symboles ieee80211_*/cfg80211_*/nl80211) et le firmware embarque
# `cfg80211.ko` + `mac80211.ko` ⇒ pas besoin du hostapd modifié Realtek.
#
# Recette (7 pièges résolus, tous reproductibles) :
#  1. tarballs séparés : depuis 2.11, hostapd N'EST PLUS dans wpa_supplicant-*.tar.gz
#     => télécharger hostapd-2.11.tar.gz à part.
#  2. libnl3 est indispensable (driver nl80211) : cross-compilé en **statique**, avec
#     `-D_TIME_BITS=32` (sinon aliases glibc __sendmsg64/__recvmsg64 absents d'uClibc)
#     et `-fno-stack-protector` (uClibc n'a pas __stack_chk_guard).
#  3. hostapd lit `CFLAGS += $(EXTRA_CFLAGS)` : ne PAS passer CFLAGS= sur la ligne de
#     commande (sinon les -I../src du Makefile sont écrasés → "utils/includes.h absent").
#  4. pas d'OpenSSL dans le sysroot ⇒ `CONFIG_TLS=internal` (+`CONFIG_INTERNAL_LIBTOMMATH=y`)
#     et EAP/HS20/WPS/DPP désactivés (AP WPA2-PSK n'en a pas besoin).
#  5. le SDK arm-linux-gnueabi d'Ubuntu est en **time64** : compiler tout avec
#     `-D_TIME_BITS=32` sinon `__isoc23_*` et `__sendmsg64` manquent.
#  6. uClibc n'exporte pas `__libc_start_main` mais `__uClibc_main` ⇒ réutiliser
#     `onvif-cam/crt0.S` + `-nostartfiles`.
#  7. symboles glibc absents d'uClibc fournis par `uclibc_compat.c`
#     (__assert_fail, __isoc23_strto*/sscanf/fscanf, __ctype_*_loc, newlocale/strerror_l).
#
# Le binaire ne dépend QUE de libc.so.0 (uClibc) — vérifié avec objdump -p.
#
# Usage : bash build.sh   (sortie dans ./out/hostapd ; test caméra = ./test_on_camera.sh <IP>)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/src"
OUT="$HERE/out"
P5=/tmp/p5build
UCLIB=${UCLIB:-/tmp/ucbuild/uclibc}
ROOTFS=${ROOTFS:-/tmp/rootfs}
CC=${CC:-arm-linux-gnueabi-gcc}
ISO="-D__isoc23_strtol=strtol -D__isoc23_strtoul=strtoul -D__isoc23_strtoll=strtoll \
     -D__isoc23_strtoull=strtoull"
mkdir -p "$OUT" "$P5"
# telecharge les tarballs si absents (le depot public ne les embarque pas)
fetch() {  # fetch <fichier> <url>
    [ -f "$SRC/$1" ] && return 0
    echo "[*] telechargement de $1"
    ( wget -q -O "$SRC/$1" "$2" || curl -fsSL -o "$SRC/$1" "$2" ) || {
        echo "KO: impossible de recuperer $1 ($2)"; exit 1; }
}
fetch hostapd-2.11.tar.gz        "https://w1.fi/releases/hostapd-2.11.tar.gz"
fetch libnl-3.11.0.tar.gz        "https://github.com/thom311/libnl/releases/download/libnl3_11_0/libnl-3.11.0.tar.gz"
[ -f "$SRC/hostapd-2.11.tar.gz" ] || { echo "KO: $SRC/hostapd-2.11.tar.gz manquant"; exit 1; }
[ -f "$SRC/libnl-3.11.0.tar.gz" ] || { echo "KO: $SRC/libnl-3.11.0.tar.gz manquant"; exit 1; }

echo "=== [1/4] libnl 3.11 (statique, time32, sans stack-protector) ==="
cd "$P5"
[ -d libnl-3.11.0 ] || tar xzf "$SRC/libnl-3.11.0.tar.gz"
cd libnl-3.11.0
./configure --host=arm-linux-gnueabi --prefix="$P5/libnl-inst" \
    --enable-shared=no --enable-static=yes --disable-cli \
    CFLAGS="-D_TIME_BITS=32 -Os -fno-stack-protector -U_FORTIFY_SOURCE -march=armv5te -mfloat-abi=soft" \
    >/tmp/p5_libnl_cfg.log 2>&1
make -j"$(nproc)" >/tmp/p5_libnl_build.log 2>&1
make install >/dev/null 2>&1
echo "   libnl OK -> $P5/libnl-inst/lib"

echo "=== [2/4] hostapd 2.11 : configuration ==="
cd "$P5"
[ -d hostapd-2.11 ] || tar xzf "$SRC/hostapd-2.11.tar.gz"
cd hostapd-2.11/hostapd
cp -f defconfig .config
sed -i 's/^#CONFIG_DRIVER_NL80211=y/CONFIG_DRIVER_NL80211=y/'   .config
sed -i 's/^#CONFIG_IEEE80211N=y/CONFIG_IEEE80211N=y/'           .config
sed -i 's/^#CONFIG_IEEE80211AC=y/CONFIG_IEEE80211AC=y/'         .config
sed -i 's/^#CONFIG_LIBNL32=y/CONFIG_LIBNL32=y/'                 .config
sed -i 's/^#CONFIG_TLS=openssl/CONFIG_TLS=internal/'            .config
sed -i 's/^#CONFIG_INTERNAL_LIBTOMMATH=y/CONFIG_INTERNAL_LIBTOMMATH=y/' .config
sed -i 's/^CONFIG_EAP_/#CONFIG_EAP_/; s/^CONFIG_HS20/#CONFIG_HS20/; s/^CONFIG_WPS/#CONFIG_WPS/; \
        s/^CONFIG_INTERWORKING/#CONFIG_INTERWORKING/; s/^CONFIG_DPP/#CONFIG_DPP/' .config
grep -E "^CONFIG_(DRIVER_NL80211|LIBNL32|TLS|INTERNAL_LIBTOMMATH)=" .config

echo "=== [3/4] compat uClibc + crt0 ==="
$CC -march=armv5te -mfloat-abi=soft -Os -fno-stack-protector -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
    -c "$HERE/uclibc_compat.c" -o /tmp/uclibc_compat.o
$CC -march=armv5te -mfloat-abi=soft -c "$HERE/../onvif-cam/crt0.S" -o /tmp/p5_crt0.o 2>/dev/null || \
$CC -march=armv5te -mfloat-abi=soft -c "$HERE/../../onvif-cam/crt0.S" -o /tmp/p5_crt0.o

echo "=== [4/4] link ==="
export EXTRA_CFLAGS="-march=armv5te -mfloat-abi=soft -Os -D_TIME_BITS=32 $ISO \
  -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -fno-stack-protector -I$P5/libnl-inst/include/libnl3 \
  -I$PWD/../src -I$PWD/../src/utils -I$PWD/../src/common -I$PWD/../src/ap"
export LIBS="/tmp/p5_crt0.o /tmp/uclibc_compat.o -L$P5/libnl-inst/lib -lnl-3 -lnl-genl-3 -lc"
make clean >/dev/null 2>&1 || true
make hostapd CC="$CC" \
  LDFLAGS="-nostartfiles -L$P5/libnl-inst/lib -L$UCLIB -Wl,-rpath-link,$P5/libnl-inst/lib \
           -Wl,-rpath-link,$ROOTFS/lib -Wl,--dynamic-linker=/lib/ld-uClibc.so.0 \
           -Wl,--allow-shlib-undefined" 2>&1 | grep -vE "^  CC " | tail -4
arm-linux-gnueabi-strip hostapd -o "$OUT/hostapd"
echo "=== RÉSULTAT ==="
file "$OUT/hostapd"; ls -l "$OUT/hostapd"; md5sum "$OUT/hostapd"
arm-linux-gnueabi-objdump -p "$OUT/hostapd" | grep -E "NEEDED|interpreter"
