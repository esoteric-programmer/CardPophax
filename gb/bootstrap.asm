; ------------------------------------------------------------------------
; bootstrap.asm — Stage-0 bootstrap for the homebrew loader (docs/05-loader-protocol.md §7)
;
; Uploaded (<=512 B) into $C000 via the TCG's Card Pop! RPC cmd3 (the one WRAM
; region proven free during serve), entered with cmd4 (jp $C000). It selects
; WRAM bank 7, pulls the full loader over the mark/space IR link into $D000, and
; jumps to it. Runs with the cart still inserted, IME=0.
;
; The screen turns BOOT_COLOR (teal) once, when the bootstrap starts. A DEBUG
; build (make DEBUG=1, -D SCREEN_STATUS) also recolours it per chunk: teal = a
; chunk passed its checksum, red = a copy failed (recvstream.inc).
;
; Built with a LOAD block so labels resolve to $C000 while the image lives in
; ROM (same trick Snake/the dumper use); extract the "boot" section from ROM
; offset $150. See Makefile.
; ------------------------------------------------------------------------
INCLUDE "hardware.inc"
DEF NO_RLE EQU 1              ; loader feed is always raw (see recvstream.inc)

SECTION "wrap", ROM0[$150]
LOAD "boot", WRAM0[BOOT_ORG]

boot_start::
	di
	xor a                     ; lock the cart's save RAM first: nothing from here
	ld [MBC_RAMG], a          ; on (bootstrap/loader/payload bug) can hit the save
	; No speed switch: cmd 4 runs inside the TCG's IR transaction, which
	; IR_Begin put in CGB normal speed -- the speed the link timing assumes.
	; A bootstrap for a game that is in double speed here must switch down.
IF DEF(SCREEN_STATUS)
	xor a
	ldh [hQuiet], a
	ld a, LOW(BOOT_COLOR)
	ldh [hScreen], a
ELSE
	ld a, LOW(BOOT_COLOR)
ENDC
	call set_screen_color     ; whole screen -> BOOT_COLOR: "it's running" (uses
	                          ; the TCG's stack; still valid here)
	ld a, IR_OFF              ; receiver enabled, LED off
	ldh [rRP], a
	ld a, LOADER_BANK         ; map WRAM bank 7 at $D000 for the loader
	ldh [rSVBK], a
	ld sp, LOADER_SP          ; stack in bank 7 (HRAM holds recvstream state)
	xor a
	ldh [hLastId], a          ; nothing received yet (IDs are never 0)

	; Multi-copy feed with per-copy ACK (recvstream.inc): take the first copy
	; of stream 'L' whose checksum matches, ACK it, jump.
	ld hl, LOADER_ORG
	ld b, HIGH(LOADER_MAX)     ; loader must end below the menu buffers
	ld c, ID_LOADER
	call recv_checked_stream
	jp LOADER_ORG

INCLUDE "recvstream.inc"
INCLUDE "bootscreen.inc"
INCLUDE "irbit.asm"

ENDL
