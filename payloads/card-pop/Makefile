# Card Pop! gift for the Pokémon TCG — a homebrew-launch payload (not in the
# default ATtiny set; pick it with `make PAYLOADS="... card-pop"` in ../../attiny).
# A: add a Mew, B: add a Venusaur, recorded like a real Card Pop!.
#
#   make               card-pop.gbc  (a bootable 32 KB ROM: a ROM loader copies
#                                     the payload to WRAM $C000)
#   make card-pop.bin  the $C000 payload image, for `gbcpop attiny-inc`
#   make tests         fake TCG carts (the payload patches its own cartridge,
#                      so an emulator needs no swap) — see test_card_pop.py
RGBASM  ?= rgbasm
RGBLINK ?= rgblink
RGBFIX  ?= rgbfix

all: card-pop.gbc
tests: test-TCG.gbc test-TCGJ.gbc
.PHONY: all tests clean

# The payload build draws with the menu font the loader left in VRAM; the
# standalone ROM and the test carts carry the font themselves.
card-pop-pl.o: card-pop.asm
	$(RGBASM) -o $@ $<
card-pop.o: card-pop.asm
	$(RGBASM) -DSTANDALONE -o $@ $<

card-pop.gbc: card-pop.o
	$(RGBLINK) -o $@ $<
	$(RGBFIX)  -C -v -p 0xFF -t CARDPOP $@

card-pop-pl.gbc: card-pop-pl.o
	$(RGBLINK) -o $@ -m card-pop.map $<
	$(RGBFIX)  -C -v -p 0xFF -t CARDPOP $@

# The payload is the "wram" LOAD section (Payload..PayloadEnd in the map). It is
# placed in the ROM at the physical offset of the `Payload` label; extract that
# many bytes from there.
card-pop.bin: card-pop-pl.gbc
	@off=$$(sed -nE 's/^[[:space:]]*\$$([0-9A-Fa-f]+) = Payload$$/\1/p' card-pop.map); \
	 end=$$(sed -nE 's/^[[:space:]]*\$$([0-9A-Fa-f]+) = PayloadEnd$$/\1/p' card-pop.map); \
	 len=$$((0x$$end - 0xc000)); \
	 dd if=card-pop-pl.gbc bs=1 skip=$$((0x$$off)) count=$$len of=$@ status=none; \
	 echo "$@ = $$len bytes (payload @ ROM \$$$$off)"

# Same program with the TCG header (US: POKECARD / AXQE, MBC5+RAM+BATTERY,
# 32 KB SRAM); TCGJ has the Japanese destination and must be refused.
TCG_FIX = -c -v -p 0xFF -t POKECARD -i AXQE -k 01 -l 0x33 -m 0x1B -r 3
test-TCG.gbc: card-pop.o
	$(RGBLINK) -o $@ $<
	$(RGBFIX)  $(TCG_FIX) -j $@
test-TCGJ.gbc: card-pop.o
	$(RGBLINK) -o $@ $<
	$(RGBFIX)  $(TCG_FIX) $@

clean:
	rm -f *.o card-pop.gbc card-pop-pl.gbc card-pop.map card-pop.bin test-*.gbc
