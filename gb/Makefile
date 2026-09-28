# GB side: Stage-0 bootstrap, resident loader, and the headless test builds.
#   make            bootstrap.bin + loader.bin + loader_pi.bin
#                   (loader_pi.bin: the Raspberry Pi launcher's loader with the
#                   scrolling payload list, -D PI_MENU; the ATtiny uses loader.bin)
#   make DEBUG=1    the same, the bootstrap recolouring the screen per chunk
#                   (SCREEN_STATUS: teal = chunk OK, red = copy failed)
#   make tests      loader_test.bin (auto-picks payload 1) + boottest.gb
RGBASM  ?= rgbasm
RGBLINK ?= rgblink
RGBFIX  ?= rgbfix

all: bootstrap.bin loader.bin loader_pi.bin
tests: loader_test.bin boottest.gb
.PHONY: all tests clean FORCE

COMMON = irbit.asm recvstream.inc hardware.inc bootscreen.inc

# The code is linked at its WRAM address with a LOAD block inside a ROM0
# section at $150; extract that section from the ROM image.
define extract
	@n=$$(grep '"$(2)"' $(1).map | sed -E 's/.*\(\$$([0-9A-Fa-f]+) bytes.*/\1/'); \
	  dd if=$(1).gb bs=1 skip=$$((0x150)) count=$$((0x$$n)) of=$@ status=none; \
	  echo "$@ = $$((0x$$n)) bytes"
endef

# rebuilt every time: DEBUG changes it without changing a source
bootstrap.bin: bootstrap.asm $(COMMON) FORCE
	$(RGBASM)  $(if $(DEBUG),-D SCREEN_STATUS) -o bootstrap.o bootstrap.asm
	$(RGBLINK) -o bootstrap.gb -m bootstrap.map bootstrap.o
	$(call extract,bootstrap,boot)
	@test $$(stat -c %s $@) -le 512 || { echo "!! bootstrap > 512 B (Stage-0 limit)"; exit 1; }
	@rm -f bootstrap.o bootstrap.gb bootstrap.map

loader.bin: loader.asm $(COMMON)
	$(RGBASM)  -o loader.o loader.asm
	$(RGBLINK) -o loader.gb -m loader.map loader.o
	$(call extract,loader,loader)
	@rm -f loader.o loader.gb loader.map

loader_pi.bin: loader.asm $(COMMON)
	$(RGBASM)  -DPI_MENU -o loader_pi.o loader.asm
	$(RGBLINK) -o loader_pi.gb -m loader_pi.map loader_pi.o
	$(call extract,loader_pi,loader)
	@rm -f loader_pi.o loader_pi.gb loader_pi.map

loader_test.bin: loader.asm $(COMMON)
	$(RGBASM)  -DTEST_CHOICE=1 -o lt.o loader.asm
	$(RGBLINK) -o lt.gb -m lt.map lt.o
	$(call extract,lt,loader)
	@rm -f lt.o lt.gb lt.map

boottest.gb: boottest.asm $(COMMON)
	$(RGBASM)  -o boottest.o boottest.asm
	$(RGBLINK) -o $@ boottest.o
	$(RGBFIX)  -C -v -p 0xFF -t BOOTTEST $@
	@rm -f boottest.o

clean:
	rm -f *.o *.gb *.map *.bin
