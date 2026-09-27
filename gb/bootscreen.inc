; bootscreen.inc — whole-screen colour cue (bootstrap and boottest).
; set_screen_color: fill all 8 CGB BG palettes with the colour whose BGR555 low
; and high byte both equal a, and hide sprites, so the whole screen turns one
; colour without touching VRAM (tiles/attributes stay whatever the game left;
; the menu resets them later). Palette RAM is only writable outside mode 3, so
; this waits for the start of vblank (up to ~17 ms). Assumes the LCD is on (it
; is at Stage 0; with it off LY freezes -> hang). Clobbers a, b, c.
set_screen_color:
	ld c, a
.bs_wait
	ldh a, [rLY]
	cp 144
	jr nz, .bs_wait            ; first line of vblank -> the whole ~1 ms budget
	ldh a, [rLCDC]
	res 1, a                   ; sprites off
	ldh [rLCDC], a
	ld a, $80                  ; BG palette 0 colour 0, auto-increment
	ldh [rBCPS], a
	ld b, 8 * 4 * 2            ; 8 palettes x 4 colours x 2 bytes
.bs_fill
	ld a, c
	ldh [rBCPD], a
	dec b
	jr nz, .bs_fill
	ret
