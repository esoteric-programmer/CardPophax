# gbcpop — Pokémon TCG (GBC) infrared tool.
#
# On the PC, build it with SHM=1: that generates the ATtiny data
# (`gbcpop attiny-inc`) and drives the VBA test bench (docs/03). The GPIO
# backends below are for a Raspberry Pi from the project's beginnings; they only
# speak the Card Pop! stage and are untested with the loader (optional/future).
#
#   make SHM=1           /dev/shm bridge to the patched VBA (PC).
#   make                 direct /dev/mem backend — no libraries at all.
#                        Builds on any Raspbian, including very old ones.
#   make WIRINGPI=1      wiringPi backend (same technique, proven library).
#   make PIGPIO=1        pigpio backend (DMA waves; nicest if available).
#
# Cross-compiling from a desktop, e.g. with an arm-linux-gnueabihf toolchain:
#   make CC=arm-linux-gnueabihf-gcc
#
# payload.bin (the resident bridge for swap-dump / swap-push, test bench only)
# is not committed; build it with `make payload.bin` (needs rgbds).

CC      ?= gcc
CFLAGS  ?= -O2 -std=c99 -Wall -Wextra
LDLIBS   = -lrt

ifdef PIGPIO
CFLAGS  += -DUSE_PIGPIO
LDLIBS  += -lpigpio -lpthread
endif
ifdef WIRINGPI
CFLAGS  += -DUSE_WIRINGPI
LDLIBS  += -lwiringPi -lpthread
endif
ifdef SHM
CFLAGS  += -DUSE_SHM
SHM_SRC  = shmring.c
endif

RGBASM  ?= rgbasm
RGBLINK ?= rgblink
PAYLOAD_IMAGE = 512          # must match DEF PAYLOAD_IMAGE in payload.asm

all: gbcpop

gbcpop: gbcpop.c cardtable.h $(SHM_SRC)
	$(CC) $(CFLAGS) -o $@ gbcpop.c $(SHM_SRC) $(LDLIBS)

# Host build for the offline subcommands (predict, forge, cards) — no GPIO.
gbcpop-offline: gbcpop.c cardtable.h stub/pigpio.h
	$(CC) $(CFLAGS) -DUSE_PIGPIO -Istub -o $@ gbcpop.c

payload.bin: payload.asm
	$(RGBASM) -o payload.o payload.asm
	$(RGBLINK) -o payload.gb payload.o
	head -c $(PAYLOAD_IMAGE) payload.gb > $@
	@rm -f payload.o payload.gb
	@echo "payload.bin: $(PAYLOAD_IMAGE) bytes"

clean:
	rm -f gbcpop gbcpop-offline payload.o payload.gb

distclean: clean
	rm -f payload.bin

.PHONY: all clean distclean
