# Snake for the Game Boy Color — a homebrew-launch payload and a standalone ROM.
#
#   make            snake.gbc   (a bootable 32 KB ROM: a ROM loader copies the
#                                game to WRAM $C000 and jumps there)
#   make snake.bin  snake.bin   (just the $C000 payload image, for the loader /
#                                `gbcpop attiny-inc`)
RGBASM  ?= rgbasm
RGBLINK ?= rgblink
RGBFIX  ?= rgbfix

all: snake.gbc
.PHONY: all clean

# The payload build returns to the loader's menu on SELECT; the standalone ROM
# has no loader to return to.
snake.gbc: snake.asm
	$(RGBASM)  -DSTANDALONE -o snake.o snake.asm
	$(RGBLINK) -o $@ snake.o
	$(RGBFIX)  -C -v -p 0xFF -t SNAKE $@

snake-pl.gbc: snake.asm
	$(RGBASM)  -o snake-pl.o snake.asm
	$(RGBLINK) -o $@ -m snake.map snake-pl.o
	$(RGBFIX)  -C -v -p 0xFF -t SNAKE $@

# The payload is the "wram" LOAD section (Payload..PayloadEnd in the map). It is
# placed in the ROM at the physical offset of the `Payload` label; extract that
# many bytes from there.
snake.bin: snake-pl.gbc
	@off=$$(sed -nE 's/^[[:space:]]*\$$([0-9A-Fa-f]+) = Payload$$/\1/p' snake.map); \
	 end=$$(sed -nE 's/^[[:space:]]*\$$([0-9A-Fa-f]+) = PayloadEnd$$/\1/p' snake.map); \
	 len=$$((0x$$end - 0xc000)); \
	 dd if=snake-pl.gbc bs=1 skip=$$((0x$$off)) count=$$len of=$@ status=none; \
	 echo "$@ = $$len bytes (payload @ ROM \$$$$off)"

clean:
	rm -f snake.o snake-pl.o snake.gbc snake-pl.gbc snake.map snake.sym snake.bin
