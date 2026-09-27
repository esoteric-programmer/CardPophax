; Snake for Game Boy Color — runs entirely from WRAM/HRAM.
; A tiny ROM loader copies the game to $C000 and jumps there; after that
; the cartridge is never touched again (no ROM reads, no interrupt vectors),
; so it can be pulled once the game is running.
;
; Launched by the homebrew loader (snake.bin), SELECT returns to its menu. The
; standalone ROM (snake.gbc, -DSTANDALONE) has no loader to return to.
;
; Build: see Makefile.

DEF rP1   EQU $FF00
DEF rDIV  EQU $FF04
DEF rIF   EQU $FF0F
DEF rLCDC EQU $FF40
DEF rSTAT EQU $FF41
DEF rSCY  EQU $FF42
DEF rSCX  EQU $FF43
DEF rLY   EQU $FF44
DEF rVBK  EQU $FF4F
DEF rBCPS EQU $FF68
DEF rBCPD EQU $FF69
DEF rSVBK EQU $FF70
DEF rIE   EQU $FFFF
DEF LOADER_RETURN EQU $D003 ; the loader's way back to its menu (WRAM bank 7)

DEF MAP     EQU $C400       ; WRAM shadow of the 32x32 BG map
DEF T_EMPTY EQU 0           ; tile indices == shadow map values
DEF T_WALL  EQU 1
DEF T_FOOD  EQU 2
DEF T_SNAKE EQU 3           ; 3..6: body; low 2 bits = direction to next segment
DEF NTILES  EQU 7
DEF SPEED   EQU 10          ; frames per step at start (min 3)

; Directions: 0 right, 1 left, 2 up, 3 down (== joypad bit order, opposite = xor 1)

SECTION "vars", HRAM
hHead:  ds 2
hTail:  ds 2
hDir:   ds 1
hNext:  ds 1
hTimer: ds 1
hSpeed: ds 1
hRng:   ds 1

SECTION "rom", ROM0[$100]
    nop
    jp Loader
    ds $150 - @, 0          ; cartridge header, filled in by rgbfix

Loader:
    di
    ld hl, Payload
    ld de, Start
    ld bc, PayloadEnd - Start
.copy
    ld a, [hli]
    ld [de], a
    inc de
    dec bc
    ld a, b
    or c
    jr nz, .copy
    jp Start

Payload:
LOAD "wram", WRAM0[$C000]
Start:
    ld a, 1                 ; VBlank only wakes HALT; IME stays 0, no vectors used
    ldh [rIE], a

Restart:
    ; LCD off (only allowed during VBlank)
:   ldh a, [rLY]
    cp 144
    jr c, :-
    xor a
    ldh [rLCDC], a
    ldh [rSCX], a
    ldh [rSCY], a

    ; generate tiles: solid color min(t,3) with a 1px gap right/bottom
    ld hl, $8000
    ld c, 0
.tile
    ld a, c
    cp 3
    jr c, :+
    ld a, 3
:   ld b, a
    rra
    sbc a, a
    and $FE
    ld d, a                 ; low bitplane
    ld a, b
    rra
    rra
    sbc a, a
    and $FE
    ld e, a                 ; high bitplane
    ld b, 7
:   ld a, d
    ld [hli], a
    ld a, e
    ld [hli], a
    dec b
    jr nz, :-
    xor a
    ld [hli], a
    ld [hli], a
    inc c
    ld a, c
    cp NTILES
    jr nz, .tile

    ; BG palette 0
    ld a, $80
    ldh [rBCPS], a
    ld hl, Palette
    ld b, 8
:   ld a, [hli]
    ldh [rBCPD], a
    dec b
    jr nz, :-

    ; shadow map: all wall, then clear the 18x16 interior
    ld hl, MAP
:   ld [hl], T_WALL
    inc hl
    ld a, h
    cp HIGH(MAP + $400)
    jr nz, :-
    ld hl, MAP + 33
    ld de, 32 - 18
    ld c, 16
.row
    ld b, 18
    xor a
:   ld [hli], a
    dec b
    jr nz, :-
    add hl, de
    dec c
    jr nz, .row

    ; snake of length 3 heading right
    ld hl, MAP + 8 * 32 + 5
    ld a, l
    ldh [hTail], a
    ld a, h
    ldh [hTail + 1], a
    ld a, T_SNAKE
    ld [hli], a
    ld [hli], a
    ld [hl], a
    ld a, l
    ldh [hHead], a
    ld a, h
    ldh [hHead + 1], a
    xor a
    ldh [hDir], a
    ldh [hNext], a
    ld a, SPEED
    ldh [hSpeed], a
    ldh [hTimer], a
    call PlaceFood

    ; clear attribute map (VRAM bank 1), then copy shadow to BG map
    ld a, 1
    ldh [rVBK], a
    ld hl, $9800
    xor a
:   ld [hli], a
    bit 2, h                ; until $9C00
    jr z, :-
    ldh [rVBK], a
    ld hl, MAP
    ld de, $9800
:   ld a, [hli]
    ld [de], a
    inc de
    bit 2, d
    jr z, :-

    ld a, $91               ; LCD on, tiles at $8000, map at $9800, BG on
    ldh [rLCDC], a

Main:
    call WaitVBlank
    ld a, $20               ; select d-pad
    ldh [rP1], a
    ldh a, [rP1]
    ldh a, [rP1]
    cpl
    and $0F
    jr z, .nokey
    ld c, -1
:   inc c                   ; c = lowest pressed direction
    rra
    jr nc, :-
    ldh a, [hDir]
    xor c
    dec a                   ; reversing into yourself is ignored
    jr z, .nokey
    ld a, c
    ldh [hNext], a
.nokey
    ld hl, hTimer
    dec [hl]
    jr nz, Main
    ldh a, [hSpeed]
    ld [hl], a

    ; step: mark current head with the move direction, advance
    ldh a, [hNext]
    ldh [hDir], a
    ld c, a
    ldh a, [hHead]
    ld l, a
    ldh a, [hHead + 1]
    ld h, a
    ld a, c
    add T_SNAKE
    ld [hl], a
    call Step
    ld a, [hl]
    cp T_FOOD
    jr z, .eat
    or a
    jr nz, Dead

    ; move tail forward
    push hl
    ldh a, [hTail]
    ld l, a
    ldh a, [hTail + 1]
    ld h, a
    ld a, [hl]
    sub T_SNAKE
    ld c, a
    xor a
    call PutTile
    call Step
    ld a, l
    ldh [hTail], a
    ld a, h
    ldh [hTail + 1], a
    pop hl
    jr .head

.eat
    push hl
    call PlaceFood
    pop hl
    ldh a, [hSpeed]
    cp 4
    jr c, .head
    dec a
    ldh [hSpeed], a

.head
    ld a, l
    ldh [hHead], a
    ld a, h
    ldh [hHead + 1], a
    ld a, T_SNAKE
    call PutTile
    jr Main

Dead:
    ld c, 90
:   call WaitVBlank
    dec c
    jr nz, :-
    jp Restart

WaitVBlank:
    xor a
    ldh [rIF], a
    halt                    ; IME=0: wakes on VBlank without jumping to a vector
IF !DEF(STANDALONE)
    ld a, $10               ; SELECT (every frame): back to the loader's menu
    ldh [rP1], a
    ldh a, [rP1]
    ldh a, [rP1]
    bit 2, a
    ret nz
    ld a, 7
    ldh [rSVBK], a
    jp LOADER_RETURN
ELSE
    ret
ENDC

; hl += Offsets[c]
Step:
    ld a, c
    add a
    add LOW(Offsets)
    ld e, a
    ld d, HIGH(Offsets)
    ld a, [de]
    inc e
    add l
    ld l, a
    ld a, [de]
    adc h
    ld h, a
    ret

PlaceFood:
    call Rand
    ld l, a
    call Rand
    and 3
    add HIGH(MAP)
    ld h, a
    ld a, [hl]
    or a
    jr nz, PlaceFood
    ld a, T_FOOD
    ; fall through

; write tile a at shadow address hl and to the BG map (safe with LCD on)
PutTile:
    ld [hl], a
    ld b, a
    ld a, h
    sub HIGH(MAP) - $98
    ld d, a
    ld e, l
:   ldh a, [rSTAT]
    and 2
    jr nz, :-
    ld a, b
    ld [de], a
    ret

Rand:
    ldh a, [hRng]
    ld b, a
    add a
    add a
    add b
    inc a                   ; x*5+1
    ldh [hRng], a
    ld b, a
    ldh a, [rDIV]
    xor b
    ret

Offsets:
    dw 1, -1, -32, 32
ASSERT HIGH(Offsets) == HIGH(Offsets + 7)

Palette:                    ; BGR555: background, wall, food, snake
    dw $0882, $39CE, $109F, $2388
PayloadEnd:
ENDL
