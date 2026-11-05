# Build croisé du démon ONVIF caméra (ARMv5 / uClibc 1.0.28)
#   make            -> lsc_onvif (binaire ARM, lié uClibc)
#   make test-host  -> binaire x86 pour tests rapides de logique XML
# NB: make prédéfinit CC=cc (origine « default ») => ?= ne s'applique pas.
ifeq ($(origin CC),default)
CC      := arm-linux-gnueabi-gcc
endif
ifeq ($(origin STRIP),undefined)
STRIP   := arm-linux-gnueabi-strip
endif
UCLIB   ?= /tmp/ucbuild/uclibc
ROOTFS  ?= /tmp/rootfs
CFLAGS  = -Os -std=gnu17 -D_TIME_BITS=32 -D__NO_CTYPE \
          -march=armv5te -mfloat-abi=soft -static-libgcc -nostartfiles \
          -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -fno-stack-protector \
          -ffunction-sections -fdata-sections -Wall -Wextra -Wno-unused-parameter
LDFLAGS = -Wl,--gc-sections -Wl,--dynamic-linker=/lib/ld-uClibc.so.0 \
          -Wl,-rpath-link,$(ROOTFS)/lib -Wl,--hash-style=sysv -L$(UCLIB) -lc

all: lsc_onvif
host:
	gcc -O1 -g -Wall -Wextra -Wno-unused-parameter -o lsc_onvif_host lsc_onvif.c
lsc_onvif: lsc_onvif.c crt0.S
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ crt0.S lsc_onvif.c
	$(STRIP) $@
	@ls -l $@; echo "size=$$(stat -c%s $@)"; arm-linux-gnueabi-readelf -d $@ | grep -E 'NEEDED|interp' | head -3
clean:
	rm -f lsc_onvif lsc_onvif_host
.PHONY: all clean
