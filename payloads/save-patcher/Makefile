# Save patcher — a homebrew-launch payload. A: add the Mew (mew.inc, or its Time
# Capsule form mew2.inc on Gold/Silver/Crystal); B: add the Celebi in
# celebi.inc (G/S/C); START: enable the GS Ball / Celebi event (Crystal).
#
#   make                   save-patcher.gbc  (a bootable 32 KB ROM: a ROM loader
#                                             copies the patcher to WRAM $C000)
#   make save-patcher.bin  the $C000 payload image, for `gbcpop attiny-inc`
#   make tests             fake carts (the patcher checks and patches its own
#                          cartridge, so an emulator needs no swap) — see
#                          test_patcher.py
#   make mew.inc mew2.inc MEW_SAV=x.sav [MEW_SLOT=1]   Gen-1 party Pokémon to add
#                                                 (mew2.inc: Time Capsule form)
#   make celebi.inc CELEBI_SAV=x.sav CELEBI_BOX=14 CELEBI_SLOT=13
#                                                 Gen-2 box Pokémon to add
RGBASM  ?= rgbasm
RGBLINK ?= rgblink
RGBFIX  ?= rgbfix

all: save-patcher.gbc
tests: test-BYTE.gbc test-BYTD.gbc test-BXTJ.gbc test-AAXD.gbc test-AAUE.gbc \
       test-AAUJ.gbc test-RED.gbc test-YELLOW.gbc test-REDJ.gbc
.PHONY: all tests clean

# mew.inc / celebi.inc are committed; regenerate them only on request.
ifdef MEW_SAV
mew.inc: extract_mon.py
	./extract_mon.py gen1 "$(MEW_SAV)" --slot $(or $(MEW_SLOT),1) --prefix MEW -o $@
mew2.inc: extract_mon.py
	./extract_mon.py gen1to2 "$(MEW_SAV)" --slot $(or $(MEW_SLOT),1) --prefix MEW2 -o $@
.PHONY: mew.inc mew2.inc
endif
ifdef CELEBI_SAV
celebi.inc: extract_mon.py
	./extract_mon.py gen2 "$(CELEBI_SAV)" --box $(CELEBI_BOX) --slot $(CELEBI_SLOT) \
	    --prefix CELEBI -o $@
.PHONY: celebi.inc
endif
MONS = mew.inc mew2.inc celebi.inc

# The payload build draws with the menu font the loader left in VRAM; the
# standalone ROM and the test carts carry the font themselves.
save-patcher-pl.o: save-patcher.asm $(MONS)
	$(RGBASM) -o $@ $<
save-patcher.o: save-patcher.asm $(MONS)
	$(RGBASM) -DSTANDALONE -o $@ $<

save-patcher.gbc: save-patcher.o
	$(RGBLINK) -o $@ $<
	$(RGBFIX)  -C -v -p 0xFF -t SAVEPATCH $@

save-patcher-pl.gbc: save-patcher-pl.o
	$(RGBLINK) -o $@ -m save-patcher.map $<
	$(RGBFIX)  -C -v -p 0xFF -t SAVEPATCH $@

# The payload is the "wram" LOAD section (Payload..PayloadEnd in the map). It is
# placed in the ROM at the physical offset of the `Payload` label; extract that
# many bytes from there.
save-patcher.bin: save-patcher-pl.gbc
	@off=$$(sed -nE 's/^[[:space:]]*\$$([0-9A-Fa-f]+) = Payload$$/\1/p' save-patcher.map); \
	 end=$$(sed -nE 's/^[[:space:]]*\$$([0-9A-Fa-f]+) = PayloadEnd$$/\1/p' save-patcher.map); \
	 len=$$((0x$$end - 0xc000)); \
	 dd if=save-patcher-pl.gbc bs=1 skip=$$((0x$$off)) count=$$len of=$@ status=none; \
	 echo "$@ = $$len bytes (payload @ ROM \$$$$off)"

# Same program with a Crystal header (MBC3+TIMER+RAM+BATTERY, 32 KB SRAM) and the
# given game code; BXTJ (Japanese) must be refused.
CRYSTAL_FIX = -C -v -p 0xFF -t PM_CRYSTAL -k 01 -l 0x33 -m 0x10 -r 3
test-BYT%.gbc: save-patcher.o
	$(RGBLINK) -o $@ $<
	$(RGBFIX)  $(CRYSTAL_FIX) -j -i BYT$* $@
test-BXT%.gbc: save-patcher.o
	$(RGBLINK) -o $@ $<
	$(RGBFIX)  $(CRYSTAL_FIX) -i BXT$* $@
# ... with a Gold/Silver header (CGB-compatible, MBC3+TIMER+RAM+BATTERY);
# AAUJ (Japanese) must be refused.
test-AA%.gbc: save-patcher.o
	$(RGBLINK) -o $@ $<
	$(RGBFIX)  -c -v -p 0xFF -t POKEMON_SLV -i AA$* -k 01 -l 0x33 -m 0x10 -r 3 \
	           $(if $(filter %J,$*),,-j) $@
# ... with a Red (MBC3+RAM+BATTERY) / Yellow (MBC5+RAM+BATTERY) header. The
# program needs a GBC, so these keep the CGB flag the real Red cart lacks.
test-RED.gbc: save-patcher.o
	$(RGBLINK) -o $@ $<
	$(RGBFIX)  -c -j -v -p 0xFF -t "POKEMON RED" -k 01 -l 0x33 -m 0x13 -r 3 $@
test-YELLOW.gbc: save-patcher.o
	$(RGBLINK) -o $@ $<
	$(RGBFIX)  -c -j -v -p 0xFF -t "POKEMON YELLOW" -k 01 -l 0x33 -m 0x1B -r 3 $@
test-REDJ.gbc: save-patcher.o
	$(RGBLINK) -o $@ $<
	$(RGBFIX)  -c -v -p 0xFF -t "POKEMON RED" -k 01 -l 0x33 -m 0x13 -r 3 $@

clean:
	rm -f *.o save-patcher.gbc save-patcher-pl.gbc save-patcher.map \
	      save-patcher.bin test-*.gbc
