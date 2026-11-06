#!/bin/bash
# P5 — compilation du mini serveur de provisioning `apweb` (ARMv5 / uClibc + hôte).
#
# `apweb` sert la page WiFi en mode AP (172.14.10.1), écrit /etc/config/_ak39_factory.ini
# (format natif du firmware, cf. kit/wifi_card/) puis redémarre la caméra.
# Aucune dépendance hors libc (busybox de ces builds n'a PAS httpd).
#
# Même recette de liaison que onvif-cam/lsc_onvif : en-têtes glibc + uClibc de la caméra,
# crt0.S maison (uClibc expose __uClibc_main, pas __libc_start_main), -D_TIME_BITS=32.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/out"
UCLIB=${UCLIB:-/tmp/ucbuild/uclibc}
ROOTFS=${ROOTFS:-/tmp/rootfs}
CC=${CC:-arm-linux-gnueabi-gcc}
mkdir -p "$OUT"

echo "=== [1/2] hôte (test fonctionnel) ==="
gcc -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation \
    -o "$OUT/apweb_host" "$HERE/apweb.c"
echo "   -> $OUT/apweb_host"

echo "=== [2/2] ARMv5 / uClibc ==="
[ -f /tmp/apweb_crt0.o ] || $CC -march=armv5te -mfloat-abi=soft -c "$HERE/../../onvif-cam/crt0.S" -o /tmp/apweb_crt0.o
$CC -march=armv5te -mfloat-abi=soft -Os -D_TIME_BITS=32 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
    -fno-stack-protector -Wall -Wextra -Wno-unused-parameter -nostartfiles -o "$OUT/apweb" \
    /tmp/apweb_crt0.o "$HERE/apweb.c" \
    -L"$UCLIB" -Wl,-rpath-link,"$ROOTFS"/lib -Wl,--dynamic-linker=/lib/ld-uClibc.so.0 \
    -Wl,--allow-shlib-undefined -lc
arm-linux-gnueabi-strip "$OUT/apweb"
echo "=== RÉSULTAT ==="
file "$OUT/apweb"; ls -l "$OUT/apweb"; md5sum "$OUT/apweb" "$OUT/apweb_host"
arm-linux-gnueabi-objdump -p "$OUT/apweb" | grep -E "NEEDED|interpreter"
