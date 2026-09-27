; ------------------------------------------------------------------------
; loader.asm — the full homebrew loader (Stage 1-3), runs in WRAM bank 7 ($D000)
;
; Pulled into bank 7 by the bootstrap. Draws the menu (Stage 1), requests and
; receives the chosen payload into a staging buffer over the robust mark/space
; link (Stage 2), then applies the segments and jumps (Stage 3, via an HRAM
; trampoline so it can relocate over itself). IME=0 throughout.
;
; Stage 1 receives two streams from the host — the menu font (ID 'F') and the
; menu text (ID 'M') — draws them, then waits for A / B / Start. Everything
; after the bootstrap arrives over recv_checked_stream (optionally RLE-packed,
; ACKed per copy; stream IDs in the frame header).
;
; A payload that leaves WRAM bank 7 and HRAM $FFE0+ alone may return to the
; menu: SVBK = 7, jp LOADER_RETURN ($D003). The menu is redrawn from its
; buffers; the host needs no notice (its idle loop listens for the next REQ).
; ------------------------------------------------------------------------
INCLUDE "hardware.inc"

DEF STAGE_BUF EQU $C000        ; payload staging (bank 0, free after bootstrap)
DEF MAX_SEG   EQU 6
; font stream (ID_FONT): first_code, count, count*8 1bpp rows
; menu stream (ID_MENU): {row, col, ASCII..., 0}*, $FF
DEF FONT_MAX  EQU 64           ; glyphs; tiles are ASCII-indexed ($8000 + code*16)
DEF MENU_MAX  EQU 512
DEF GIVEUP_TIMEOUTS EQU 24     ; x ~21 ms of silence after REQ -> back to the menu
; START_MARK is defined by recvstream.inc (included below)

SECTION "lwrap", ROM0[$150]
LOAD "loader", WRAMX[LOADER_ORG]   ; linked at $D000, bank 7

loader_start::
	jp loader_init             ; $D000: entry from the bootstrap

; $D003: a payload returns to the menu (SVBK = 7 first). Undo what it may have
; set up -- interrupts, sound, save RAM, IR port, LCD/VRAM (draw_menu) -- and
; forget the last chunk, so the next payload's frames are not taken as repeats.
; The payload must be in normal speed (all of ours are).
loader_return::
	ASSERT loader_return == LOADER_RETURN
	di
	ld sp, LOADER_SP
	xor a
	ld [MBC_RAMG], a           ; save RAM off
	ldh [rIE], a
	ldh [rIF], a
	ldh [rNR52], a             ; sound off (the audio dumper leaves it on)
	ldh [hLastId], a
	ldh [hRxIdle], a
	ld a, IR_OFF
	ldh [rRP], a
	call draw_menu
	jr menu_loop

loader_init:
	di
	xor a                      ; save RAM off (the bootstrap did too; cheap insurance)
	ld [MBC_RAMG], a
	ld sp, LOADER_SP
	ld a, IR_OFF
	ldh [rRP], a
	ldh a, [rLCDC]             ; the TCG's LCDC (LCD on); the trampoline restores
	ldh [hLcdc], a             ; it for the payload after the menu changed it
	xor a
	ldh [hRxIdle], a           ; normal receive: no linger timeout

	ld hl, wFont               ; the host sends these right after the loader
	ld b, HIGH(wMenu - wFont)  ; max len (pages): must not run past the buffer
	ld c, ID_FONT
	call recv_checked_stream
	ld hl, wMenu
	ld b, HIGH(MENU_MAX)
	ld c, ID_MENU
	call recv_checked_stream
	call recv_linger           ; re-ACK a repeated last menu chunk before we
	                           ; stop receiving to draw + wait for a button
	call draw_menu
menu_loop:
IF DEF(TEST_CHOICE)
	ld a, TEST_CHOICE          ; headless test: skip the joypad
ELSE
	call read_choice           ; Stage 1 -> a = 1(A) / 2(B) / 3(Start)
ENDC
	ldh [hChoice], a           ; do_transfer waits for the feed to go idle first

	call do_transfer           ; Stage 2 -> carry set: nothing came in ~0.5 s
	jr nc, .staged             ; (REQ lost, or the transfer broke off):
	call do_transfer           ; send the REQ once more by ourselves
	jr c, menu_loop            ; still nothing: the user presses again
.staged

IF DEF(TEST_CHOICE)
	call wait_link_idle        ; clear of the body ACK, so the host can frame it
	ld a, $AA                  ; heartbeat: transfer succeeded, about to apply
	call ir_send_byte
ENDC
	call apply_and_jump        ; Stage 3 -> does not return

; Wait until the IR link has been idle (light off) continuously for ~66 ms: the
; host has finished sending (the loader feed's copies), and it is not between
; two of its ~33 ms-spaced Card Pop! probes either, but in its listen phase (the
; ATtiny idle loop), so a REQ sent now is heard. Clobbers a, bc.
wait_link_idle:
	ld bc, 0                   ; consecutive-idle poll counter
.w
	ldh a, [rRP]
	bit 1, a
	jr z, .active              ; bit1=0 -> light on -> feed still active, reset
	inc bc
	ld a, b
	cp $10                     ; bc >= $1000 (4096 polls ~= 66 ms) of idle
	ret nc
	jr .w
.active
	ld bc, 0
	jr .w

; --- Stage 1: draw the menu from wFont + wMenu ------------------------------
; Glyphs go to the tile of their ASCII code ($8000 addressing), so the menu text
; is plain ASCII; tile $20 (space) and unsent codes are blank. Resets what the
; TCG left behind: CGB attributes (palette/bank/flip) -> 0, BG palette 0 -> grey
; ramp, window + sprites off, scroll 0. Clobbers a, bc, de, hl.
draw_menu:
	ldh a, [rLCDC]
	add a                      ; LCD already off (a returning payload may leave
	jr nc, .off                ; it so): LY is frozen, no vblank to wait for
.wv	ldh a, [rLY]               ; LCD off only during vblank
	cp 144
	jr c, .wv
.off
	xor a
	ldh [rLCDC], a
	ldh [rSCX], a
	ldh [rSCY], a
	inc a
	ldh [rVBK], a              ; bank 1: BG attribute map -> 0
	ld hl, $9800
	ld bc, $400
	call fill_zero
	xor a
	ldh [rVBK], a
	ld hl, $8200               ; blank tiles $20..$5F
	ld bc, $400
	call fill_zero
	ld hl, $9800               ; BG map -> all spaces
	ld bc, $400
	ld d, ' '
	call fill
	; font: wFont = first, count, rows -> tile first, 1bpp -> colour 3
	ld hl, wFont
	ld a, [hl+]
	ld e, a
	ld d, 0
REPT 4
	sla e
	rl d
ENDR
	set 7, d                   ; de = $8000 + first*16
	ld a, [hl+]                ; count*8 rows
	ld c, a
	ld b, 0
REPT 3
	sla c
	rl b
ENDR
.glyph
	ld a, b
	or c
	jr z, .pal
	ld a, [hl+]
	ld [de], a                 ; plane 0
	inc de
	ld [de], a                 ; plane 1 = same -> colour 3
	inc de
	dec bc
	jr .glyph
.pal
	ld a, $80                  ; palette 0, colour 0, auto-increment
	ldh [rBCPS], a
	ld hl, menu_pal
	ld b, 8
.pl	ld a, [hl+]
	ldh [rBCPD], a
	dec b
	jr nz, .pl
	; text: {row, col, chars..., 0}* $FF
	ld hl, wMenu
.rec
	ld a, [hl+]
	cp $FF
	jr z, .on
	and 31                     ; row/col masked so bad menu data stays in the map
	ld e, a                    ; de = row*32
	ld d, 0
REPT 5
	sla e
	rl d
ENDR
	ld a, [hl+]                ; + col + $9800
	and 31
	add e
	ld e, a
	ld a, d
	adc $98
	ld d, a
.ch	ld a, [hl+]
	or a
	jr z, .rec
	ld [de], a
	inc de
	res 2, d                   ; $9C00 -> $9800: never write past the map
	jr .ch
.on
	ld a, $91                  ; LCD on, tiles $8000, map $9800, BG on, no OBJ/win
	ldh [rLCDC], a
	ret

; fill bc bytes at hl with 0 (fill_zero) or d (fill). Clobbers a, bc, hl.
fill_zero:
	ld d, 0
fill:
	ld a, d
	ld [hl+], a
	dec bc
	ld a, b
	or c
	jr nz, fill
	ret

menu_pal:                      ; CGB BGR555: white, light grey, dark grey, black
	dw $7FFF, $5AD6, $2D6B, $0000

; --- Stage 1: wait for and read one of A / B / Start -----------------------
; A=payload 1, B=2, Start=3. Waits for all buttons to be released first.
read_choice:
.release                       ; first wait until no button is held (after a
	ld a, $10                  ; failed request the button may still be down)
	ldh [rP1], a
	ldh a, [rP1]
	ldh a, [rP1]               ; settle
	cpl
	and $0F
	jr nz, .release
.wait
	ld a, $10                  ; select the button keys
	ldh [rP1], a
	ldh a, [rP1]
	ldh a, [rP1]               ; settle
	cpl
	and $0F                    ; bit0 A, bit1 B, bit2 Select, bit3 Start
	bit 0, a
	jr nz, .a
	bit 1, a
	jr nz, .b
	bit 3, a
	jr nz, .start
	jr .wait
.a	ld a, 1
	jr .done
.b	ld a, 2
	jr .done
.start
	ld a, 3
.done
	ret

; --- Stage 2: request payload, receive manifest + segment bodies -----------
; Manifest (ID 'N') = nseg, nseg*(dest_lo,dest_hi,len_lo,len_hi), entry_lo,
; entry_hi, flags -> wManifest (nseg at [0]). Body (ID 'B') = concatenated
; segment data -> STAGE_BUF. The stream IDs keep a repeated manifest copy from
; being taken as the body.
; REQ is one unacknowledged byte: if it is lost, or the transfer breaks off,
; nothing (more) arrives. Both receives therefore give up after GIVEUP_TIMEOUTS
; of silence without an ACKed chunk (~0.5 s; longer than a host retrying one
; chunk 8 times, so we never leave a transfer that is still going) and return
; carry set -- the caller sends the REQ once more, then goes back to the menu;
; the hosts go back to listening for a REQ. Carry clear = the payload is staged.
do_transfer:
	call wait_link_idle        ; the loader feed may still be sending copies
	                           ; (esp. if the menu button was pressed early);
	                           ; wait for it to finish before REQ + payload
	ldh a, [hChoice]           ; REQ: chosen payload id -> host
	call ir_send_byte
	ld hl, wManifest
	ld b, 1                    ; < 256 bytes (wManifest is 28; stack is at $DFxx)
	ld c, ID_MANIFEST
	ld a, GIVEUP_TIMEOUTS
	call recv_checked_stream_timed
	ret c                      ; nothing came: back to the menu
	; nseg
	ld a, [wManifest]
	ldh [hNseg], a
	; entry sits right after nseg + nseg*4 table: wManifest + 1 + nseg*4
	ld hl, wManifest + 1
	ldh a, [hNseg]
	add a
	add a                      ; nseg*4
	ld c, a
	ld b, 0
	add hl, bc                 ; hl -> entry_lo
	ld a, [hl+]
	ldh [hEntry], a
	ld a, [hl]
	ldh [hEntry + 1], a
	ld hl, STAGE_BUF
	ld b, HIGH($1000)          ; staging is $C000-$CFFF
	ld c, ID_BODY
	ld a, GIVEUP_TIMEOUTS
	call recv_checked_stream_timed
	ret c                      ; broke off: back to the menu
	call recv_linger           ; re-ACK a repeated last body chunk before the
	                           ; jump, so the host's "no ACK" means "not received"
	or a                       ; clear carry (success)
	ret

; sum the manifest len fields -> de
manifest_total_len:
	ld hl, wManifest
	ldh a, [hNseg]
	ld b, a
	ld de, 0
.l	inc hl                     ; dest_lo
	inc hl                     ; dest_hi
	ld a, [hl+]                ; len_lo
	add e
	ld e, a
	ld a, [hl+]                ; len_hi
	adc d
	ld d, a
	dec b
	jr nz, .l
	ret

; --- Stage 3: run the relocator from HRAM ----------------------------------
apply_and_jump:
	ld hl, tramp_src
	ld de, $FF80
	ld bc, tramp_end - tramp_src
.cp	ld a, [hl+]
	ld [de], a
	inc de
	dec bc
	ld a, b
	or c
	jr nz, .cp
	jp $FF80

; Copied to HRAM and run there so it may overwrite any WRAM bank. Reads the
; manifest from bank 7 (SVBK still 7; fine for bank-0 / VRAM payloads), copies
; each segment STAGE_BUF -> dest with the LCD off, then jp entry (HRAM hEntry).
tramp_src:
.wv	ldh a, [$FF44]             ; wait for vblank before touching the LCD/VRAM
	cp 144
	jr c, .wv
	xor a
	ldh [$FF40], a             ; LCDC = 0 (LCD off; VRAM now writable)
	xor a
	ldh [$FF4F], a             ; VBK = 0: VRAM writes/entry assume bank 0 (the
	                           ; TCG may have left bank 1 -> invisible tiles /
	                           ; white screen; matches `gbcpop run`'s pre-jump fix)
	ld hl, wManifest + 1       ; segment table (wManifest[0] = nseg)
	ld de, STAGE_BUF           ; running source
	ldh a, [hNseg]
	ldh [hCnt], a
.seg
	ldh a, [hCnt]
	or a
	jr z, .go
	ld a, [hl+]                ; dest_lo
	ldh [hDst], a
	ld a, [hl+]                ; dest_hi
	ldh [hDst + 1], a
	ld a, [hl+]                ; len_lo
	ld c, a
	ld a, [hl+]                ; len_hi
	ld b, a                    ; bc = len
	ld a, l                    ; save manifest ptr
	ldh [hMan], a
	ld a, h
	ldh [hMan + 1], a
	ldh a, [hDst]              ; hl = dest
	ld l, a
	ldh a, [hDst + 1]
	ld h, a
.cpseg
	ld a, b
	or c
	jr z, .segdone
	ld a, [de]
	inc de
	ld [hl+], a
	dec bc
	jr .cpseg
.segdone
	ldh a, [hMan]              ; restore manifest ptr
	ld l, a
	ldh a, [hMan + 1]
	ld h, a
	ldh a, [hCnt]
	dec a
	ldh [hCnt], a
	jr .seg
.go
	ldh a, [hLcdc]             ; restore the TCG's LCDC (LCD ON) so the payload's
	ldh [$FF40], a             ; first vblank-wait (LY poll) can complete
	ldh a, [hEntry]
	ld l, a
	ldh a, [hEntry + 1]
	ld h, a
	jp hl
tramp_end:
	ASSERT tramp_end - tramp_src <= $FFE0 - $FF80, "trampoline overlaps the HRAM vars"

DEF LINGER EQU 1               ; recv_linger (recvstream.inc)
INCLUDE "recvstream.inc"
INCLUDE "irbit.asm"
ENDL

; loader RAM (bank 7) — not part of the transferred image
SECTION "lbss", WRAMX[$DE00], BANK[LOADER_BANK]
wManifest:: ds 4 * MAX_SEG + 4   ; nseg(1) + table(4*MAX_SEG) + entry(2) + flags(1)

; menu streams, decoded (below wManifest; the loader image ends well before)
SECTION "lmenu", WRAMX[$D800], BANK[LOADER_BANK]
wFont:: ds $300                  ; 3 + FONT_MAX*8 = 515 B, in whole pages for the len guard
wMenu:: ds MENU_MAX
	ASSERT 3 + FONT_MAX * 8 <= wMenu - wFont
	ASSERT LOADER_ORG + LOADER_MAX <= wFont

; Pinned high: apply_and_jump copies its ~76-byte trampoline to $FF80, so these
; vars must live clear of $FF80..$FFCB (the tramp reads hEntry/hNseg/etc. while
; running there). $FFE0+ leaves the whole low HRAM free for the trampoline.
SECTION "lhram", HRAM[$FFE0]
hChoice:: ds 1
hNseg::   ds 1
hCnt::    ds 1
hMan::    ds 2
hDst::    ds 2
hEntry::  ds 2
hLcdc::   ds 1              ; the TCG's LCDC at loader entry, restored before jp entry
hVarsEnd:
	ASSERT hVarsEnd <= hRleOn, "loader HRAM vars overlap the recvstream state"
