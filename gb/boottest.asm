; Standalone test of the Stage-0 bootstrap's IR receive path, using the robust
; preamble+START framing (the proven mark/space link). Boots at $100 (no RPC),
; skips to the START marker, reads a 2-byte little-endian length, streams that
; many bytes into $D000 (WRAM bank 7), and jumps. Host sends the blinkstub as the
; body; a correct load+jump shows as the stub's slow IR blink in the VBA TX log.
INCLUDE "hardware.inc"
DEF SCREEN_STATUS EQU 1       ; screen green/red per chunk (recvstream.inc)
DEF NO_RLE EQU 1              ; loader feed is always raw (see recvstream.inc)
SECTION "rom", ROM0[$100]
	nop
	jp start
	ds $150 - @, 0
start:
	di
	ld a, IR_OFF
	ldh [rRP], a
	ld sp, $FFFE
	xor a
	ldh [hQuiet], a
	ld a, LOW(BOOT_COLOR)
	ldh [hScreen], a
	call set_screen_color
	ld a, LOADER_BANK
	ldh [rSVBK], a
	ld sp, LOADER_SP
	xor a
	ldh [hLastId], a
	; pull the loader into $D000, then jump (mirrors bootstrap)
	ld hl, LOADER_ORG
	ld b, HIGH(LOADER_MAX)
	ld c, ID_LOADER
	call recv_checked_stream
	jp LOADER_ORG
INCLUDE "recvstream.inc"
INCLUDE "bootscreen.inc"
INCLUDE "irbit.asm"
