# ATtiny85 launcher. loader_data.inc is generated from the GB binaries and the
# payloads by `gbcpop attiny-inc` (see ../docs/06-attiny-firmware.md).
#
#   make                  firmware from the current loader_data.inc (the last
#                         one generated; the committed one is the default:
#                         snake, card-pop, audio-dumper)
#   make PAYLOADS="snake save-patcher" [flash]
#                         rebuild loader_data.inc first, with these payloads on
#                         the menu buttons A, B, START (1 to 3 of them). Each
#                         name is a folder in ../payloads: its manifest.txt if it
#                         has one, else <name>.bin, built with `make <name>.bin`
#                         there when it has a Makefile. The on-screen menu is
#                         generated from the same list, so its lines, buttons
#                         and prompt always match the payloads built in. The
#                         audio-dumper needs its build.sh run once
#                         (../docs/07-payloads.md).
#   make DEBUG=1 [PAYLOADS=...] [flash]
#                         debug build: EEPROM trace (make eeprom-dump) and a
#                         bootstrap that colours the GBC screen per chunk. Its
#                         data goes to loader_data_debug.inc (default payloads
#                         unless PAYLOADS is given); the release data stays.
AVRA     ?= avra
AVRDUDE  ?= avrdude
PORT     ?= /dev/ttyACM0
PROG     ?= stk500v2

DATA = $(if $(DEBUG),loader_data_debug.inc,loader_data.inc)
ifdef DEBUG
PAYLOADS ?= snake card-pop audio-dumper
endif

# reassembled every time: DEBUG changes it without changing a source
main-tcg-loader.hex: main-tcg-loader.asm $(DATA) FORCE
	$(AVRA) $(if $(DEBUG),-D DEBUG) main-tcg-loader.asm   # tn85def.inc comes with avra

# the SHM build of gbcpop (cd ../host && make SHM=1)
GBCPOP   ?= ../host/gbcpop

ifdef PAYLOADS
pl_file = $(if $(wildcard ../payloads/$(1)/manifest.txt),../payloads/$(1)/manifest.txt,../payloads/$(1)/$(1).bin)

$(DATA):
	@test -x $(GBCPOP) || { echo "build gbcpop first: cd ../host && make SHM=1"; exit 1; }
	$(MAKE) -C ../gb all $(if $(DEBUG),DEBUG=1)
	@set -e; for p in $(PAYLOADS); do \
	  test -d ../payloads/$$p || { echo "no payload folder ../payloads/$$p"; exit 1; }; \
	  if [ -f ../payloads/$$p/Makefile ]; then $(MAKE) -C ../payloads/$$p $$p.bin; fi; \
	done
	$(GBCPOP) attiny-inc $@ ../gb/bootstrap.bin ../gb/loader.bin \
	    $(foreach p,$(PAYLOADS),$(call pl_file,$(p)))
.PHONY: $(DATA)
endif

# Factory fuses are what the firmware expects (lfuse 0x62 hfuse 0xdf efuse 0xff).
flash: main-tcg-loader.hex
	$(AVRDUDE) -c $(PROG) -p t85 -P $(PORT) -U flash:w:$<:i

eeprom-dump:
	$(AVRDUDE) -c $(PROG) -p t85 -P $(PORT) -U eeprom:r:eeprom.bin:r

clean:
	rm -f *.hex *.obj *.cof

.PHONY: flash eeprom-dump clean FORCE
