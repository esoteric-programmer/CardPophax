; Save patcher for Pokémon Red/Blue/Yellow, Gold/Silver and Crystal (Western
; releases) — runs entirely from WRAM/HRAM. The user swaps in a cartridge and
; presses START; the patcher identifies it and offers what it can do:
;
;   A       add the Mew (R/B/Y: mew.inc; G/S/C: mew2.inc, the same Mew as the
;           Time Capsule converts it, holding a Bitter Berry)
;   B       add the Celebi in celebi.inc (G/S/C)
;   START   enable the GS Ball / Celebi event (Crystal)
;   SELECT  back to the "insert a cartridge" prompt
; Only the buttons that apply are shown and accepted.
;
; Add to party (all generations, one routine driven by Layout + Mon blocks): the
; save must exist and pass its checksum; refused only if the party is full (the
; same species may be added again). The Pokémon goes to the first free slot
; (species + $FF terminator, count, struct, OT name, nickname), its Pokédex
; owned + seen bits are set and the checksum is resealed, then read back.
;   Gen 1 (pokered sram.asm, bank 1): party $AF2C (count, 6 species + $FF,
;     6x44 structs, 6x11 OT names, 6x11 nicknames), Pokédex owned $A5A3 / seen
;     $A5B6 (19 bytes each), one 8-bit checksum at $B523 = complement of the
;     byte sum of $A598..$B522. Red/Blue (MBC3) and Yellow (MBC5) share it.
;   Gen 2 (pokegold / pokecrystal sram.asm, bank 1): same party shape with 48-
;     byte structs, 32-byte Pokédex flag arrays, check value 99 at $A008 and a
;     16-bit little-endian byte sum of sGameData ($A009..): G/S party $A88A,
;     dex $AA4C/$AA6C, sum up to $AD68 stored at $AD69; Crystal party $A865,
;     dex $AA27/$AA47, sum up to $AB82 stored at $AD0D. Only the main copy is
;     patched: when its checksum is valid the game loads it and rewrites the
;     backup from it (TryLoadSaveFile).
;
; Crystal event: Crystal's GS Ball event (Goldenrod Pokécenter -> GS Ball ->
;   Kurt -> Ilex Forest shrine -> Celebi) was a Japanese Mobile System
;   distribution. The Western carts still carry it, gated by one save byte
;   outside the checksum (pokecrystal sram.asm, "SRAM Crystal Data"):
;   sMobileEventIndex at SRAM bank 1 $BE3C and its backup at $BE44. The
;   Pokécenter scene fires on MOBILE_EVENT_OBJECT_GS_BALL ($0B).
;
; All Western languages share these layouts (PKHeX SAV1Offsets.INT /
; SAV2Offsets); Japanese and Korean carts are refused. The result shows for
; ~2.5 s, then the prompt returns, so several carts can be patched. SRAM is
; only enabled while patching, never while the user swaps carts, and the
; cartridge is identified again when A/B is pressed (it may have been swapped
; at the menu). The APU stays off (see the audio-dumper patch).
;
; Build: make save-patcher.bin (the $C000 payload) / make (save-patcher.gbc,
;        standalone) / make tests && ./test_patcher.py

INCLUDE "mew.inc"           ; MEW_SPECIES, MEW_DEX, MEW_NAME, MEW_DATA (extract_mon.py)
INCLUDE "mew2.inc"          ; MEW2_*: that Mew converted as by the Time Capsule
INCLUDE "celebi.inc"        ; CELEBI_*

DEF rP1   EQU $FF00
DEF rIF   EQU $FF0F
DEF rNR52 EQU $FF26
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

; MBC3 / MBC5: the SRAM enable and bank registers are at the same addresses
DEF MBC_RAMG  EQU $0000     ; $0A enables SRAM, $00 disables it
DEF MBC_RAMB  EQU $4000     ; SRAM bank 0-3

; pokecrystal sram.asm / constants
DEF sCheckValue1       EQU $A008 ; bank 1, = 99 (Gen 2)
DEF sCheckValue2       EQU $AD0F ; bank 1, = 127 (Crystal)
DEF sMobileEventIndex       EQU $BE3C ; bank 1
DEF sMobileEventIndexBackup EQU $BE44 ; bank 1
DEF SAVE_CHECK_VALUE_1 EQU 99
DEF SAVE_CHECK_VALUE_2 EQU 127
DEF MOBILE_EVENT_OBJECT_GS_BALL EQU $0B
DEF NAME_LENGTH  EQU 11
DEF PARTY_LENGTH EQU 6

; identified cartridge (wGame)
DEF GAME_RBY EQU 1
DEF GAME_GS  EQU 2
DEF GAME_C   EQU 3

; joypad, buttons half (P1 = $10)
DEF PAD_A      EQU 1
DEF PAD_B      EQU 2
DEF PAD_SELECT EQU 4
DEF PAD_START  EQU 8

DEF FONT_FIRST  EQU $2C     ; tile index == ASCII code; ' ' is a blank tile
DEF SHOW_FRAMES EQU 150     ; result screen: 2.5 s at ~60 Hz
DEF BG_COLOR    EQU $2842   ; BGR555 dark blue

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
    di
    ld sp, $D000            ; top of WRAM bank 0, clear of the payload
    xor a
    ldh [rNR52], a          ; APU off while carts are swapped
    ldh [rVBK], a
    inc a
    ldh [rIE], a            ; VBlank only wakes HALT; IME stays 0, no vectors used

    ; LCD off (only allowed during VBlank; LY never reaches 144 if already off)
    ldh a, [rLCDC]
    add a
    jr nc, .lcdoff
:   ldh a, [rLY]
    cp 144
    jr c, :-
.lcdoff
    xor a
    ldh [rLCDC], a
    ldh [rSCX], a
    ldh [rSCY], a

IF DEF(STANDALONE)
    ; font: glyph rows (5 bits) -> tile FONT_FIRST+n, colour 3, blank top row.
    ; Under the loader the menu font is already there (same glyphs, same tiles).
    ld hl, $8000 + FONT_FIRST * 16
    ld de, Font
    ld c, (FontEnd - Font) / 7
.tile
    xor a
    ld [hli], a
    ld [hli], a
    ld b, 7
:   ld a, [de]
    inc de
    add a
    add a
    ld [hli], a
    ld [hli], a
    dec b
    jr nz, :-
    dec c
    jr nz, .tile
ENDC

    ; BG palette 0: all background colour (colour 3 is set per screen)
    ld a, $80
    ldh [rBCPS], a
    ld b, 4
:   ld a, LOW(BG_COLOR)
    ldh [rBCPD], a
    ld a, HIGH(BG_COLOR)
    ldh [rBCPD], a
    dec b
    jr nz, :-

    ; attributes (VRAM bank 1) to 0, BG map to spaces (tile $20 must be blank)
IF DEF(STANDALONE)
    ld hl, $8000 + ' ' * 16
    ld b, 16
    xor a
:   ld [hli], a
    dec b
    jr nz, :-
ENDC
    ld a, 1
    ldh [rVBK], a
    ld hl, $9800
    xor a
:   ld [hli], a
    bit 2, h                ; until $9C00
    jr z, :-
    ldh [rVBK], a
    ld hl, $9800
    ld a, ' '
:   ld [hli], a
    bit 2, h
    jr z, :-

    ld a, $91               ; LCD on, tiles at $8000, map at $9800, BG on
    ldh [rLCDC], a

Prompt:
    ld hl, ScrPrompt
    call DrawScreen
IF DEF(STANDALONE)
    ld c, PAD_START
    call WaitButton
ELSE
    ld c, PAD_START | PAD_SELECT
    call WaitButton         ; SELECT here (one level above the action menu's
    cp PAD_SELECT           ; SELECT: BACK): back to the loader's menu
    jp z, ToLoader
ENDC
    call Identify           ; -> a = GAME_*, z if unsupported
    ld hl, ScrBadCart
    jr z, Show
    ld [wGame], a

    ; action menu: "SELECT: BACK" + the lines that apply. A: Mew (all games),
    ; B: Celebi (G/S/C), START: GS Ball event (Crystal)
    ld hl, ScrMenu
    call DrawScreen
    ld a, [wGame]
    ld hl, ActRBY
    ld c, PAD_A | PAD_SELECT
    dec a
    jr z, .menu
    ld hl, ActGS
    ld c, PAD_A | PAD_B | PAD_SELECT
    dec a
    jr z, .menu
    ld hl, ActC
    ld c, PAD_A | PAD_B | PAD_START | PAD_SELECT
.menu
    call DrawLines
    call WaitButton         ; -> a = the pressed button(s) of c
    cp PAD_SELECT
    jr z, Prompt
    ld b, a
    push bc
    call Identify           ; still the same cartridge?
    pop bc
    ld hl, ScrBadCart
    jr z, Show
    ld c, a
    ld a, [wGame]
    cp c
    jr nz, Show
    bit 3, b                ; PAD_START
    jr nz, .event
    ld hl, LayoutRBY
    ld de, MonMew
    dec a
    jr z, .add
    ld hl, LayoutGS
    dec a
    jr z, :+
    ld hl, LayoutC
:   ld de, MonMew2          ; A: Mew (as converted by the Time Capsule)
    bit 1, b                ; PAD_B: Celebi
    jr z, .add
    ld de, MonCelebi
.add
    call AddMon
    jr Show
.event
    call CrystalEvent
Show:                       ; hl = result screen, ~2.5 s, then the prompt
    call DrawScreen
    ld c, SHOW_FRAMES
:   call WaitVBlank
    dec c
    jr nz, :-
    jp Prompt

; Wait until none of the buttons in c is held, then until one is pressed.
; Returns a = the pressed button(s) of c.
WaitButton:
:   call WaitVBlank
    call ReadPad
    jr nz, :-
:   call WaitVBlank
    call ReadPad
    jr z, :-
    ret

; a = held buttons (PAD_*) masked with c, z if none
ReadPad:
    ld a, $10               ; select the button keys
    ldh [rP1], a
    ldh a, [rP1]
    ldh a, [rP1]
    ldh a, [rP1]
    cpl
    and c
    ld b, a
    ld a, $30
    ldh [rP1], a
    ld a, b
    or a
    ret

IF !DEF(STANDALONE)
ToLoader:                   ; the loader's menu (SVBK = 7, jp LOADER_RETURN)
    ld a, 7                 ; WRAM bank 7: the loader, untouched by us
    ldh [rSVBK], a
    jp LOADER_RETURN
ENDC

; Identify the cartridge from its header. a = GAME_*, z if unsupported.
Identify:
    ; header checksum ($0134..$014C) must match $014D: a real, fully seated cart
    ld hl, $0134
    ld b, $014D - $0134
    xor a
:   sub [hl]
    dec a
    inc hl
    dec b
    jr nz, :-
    cp [hl]
    jr nz, .none
    ld a, [$014A]
    dec a                   ; destination 1: not the Japanese release
    jr nz, .none
    ld a, [$0149]
    cp $03                  ; 32 KB SRAM
    jr nz, .none
    ld a, [$0147]
    ld c, a                 ; c = cartridge type
    ld de, CrystalTitle
    call CheckTitle
    jr nz, .notc
    ld a, [$0143]
    cp $C0                  ; CGB only
    jr nz, .none
    ld b, GAME_C
    jr .gen2
.notc
    ld de, GSTitle
    call CheckTitle
    jr nz, .notgs
    ld a, [$0140]           ; game code AAU? (Gold) / AAX? (Silver)
    cp 'A'
    jr nz, .none
    ld b, GAME_GS
.gen2
    ld a, c
    cp $10                  ; MBC3+TIMER+RAM+BATTERY
    jr nz, .none
    ld a, [$0142]           ; language letter of the game code
    ld c, a
    ld hl, Languages
:   ld a, [hli]
    or a
    ret z                   ; not listed (a = 0, z)
    cp c
    jr nz, :-
    ld a, b
    or a
    ret
.notgs
    ld de, Gen1Title
    call CheckTitle
    jr nz, .none
    ld a, c
    cp $13                  ; Red/Blue: MBC3+RAM+BATTERY
    jr z, .rby
    cp $1B                  ; Yellow: MBC5+RAM+BATTERY
    jr nz, .none
.rby
    ld a, GAME_RBY
    or a
    ret
.none
    xor a
    ret

; z if the header at $0134 starts with the string at de (0-terminated)
CheckTitle:
    ld hl, $0134
:   ld a, [de]
    inc de
    or a
    ret z
    cp [hl]
    inc hl
    jr z, :-
    ret

; Crystal: enable the GS Ball event. Returns hl = result screen.
CrystalEvent:
    call SramOn
    ; a save must exist (main copy; a backup-only save is refused)
    ld hl, sCheckValue1
    ld de, sCheckValue2
    call CheckValues
    jp nz, NoSave
    ld hl, sMobileEventIndex
    ld a, [hl]
    cp MOBILE_EVENT_OBJECT_GS_BALL
    ld hl, ScrAlready
    jr z, SramOff
    ld a, MOBILE_EVENT_OBJECT_GS_BALL
    ld [sMobileEventIndex], a
    ld [sMobileEventIndexBackup], a
    ld hl, sMobileEventIndexBackup
    cp [hl]
    jr nz, WriteFail
    ld hl, sMobileEventIndex
    cp [hl]
    jr nz, WriteFail
Done:
    ld hl, ScrDone
    jr SramOff
WriteFail:
    ld hl, ScrWriteFail
    jr SramOff
NoSave:
    ld hl, ScrNoSave
    ; fall through
SramOff:
    xor a
    ld [MBC_RAMG], a        ; SRAM off before the user can pull the cart
    ret

SramOn:                     ; SRAM enabled, bank 1
    ld a, $0A
    ld [MBC_RAMG], a
    ld a, 1
    ld [MBC_RAMB], a
    ret

; z if [hl] == SAVE_CHECK_VALUE_1 and [de] == SAVE_CHECK_VALUE_2
CheckValues:
    ld a, [hl]
    cp SAVE_CHECK_VALUE_1
    ret nz
    ld a, [de]
    cp SAVE_CHECK_VALUE_2
    ret

MACRO ldptr                 ; ldptr hl, wVar: hl = [wVar] (little-endian)
    ld hl, \2
    ld a, [hli]
    ld h, [hl]
    ld l, a
ENDM

; Add the Pokémon (Mon block at de) to the party of the save described by the
; Layout block at hl (see the top). Returns hl = result screen.
AddMon:
    push de
    ld de, wParams
    ld b, wMonParams - wParams
    call CopyBytes
    pop hl
    ld b, wParamsEnd - wMonParams
    call CopyBytes
    call SramOn
    ld a, [wGen1]           ; Gen 2: a zeroed SRAM would pass the 16-bit sum
    or a
    jr nz, :+
    ld a, [sCheckValue1]
    cp SAVE_CHECK_VALUE_1
    jr nz, NoSave
:   call CheckSum
    jr nz, NoSave
    ldptr hl, wParty
    ld a, [hli]             ; count
    cp PARTY_LENGTH
    jr c, :+
    jr nz, NoSave           ; > 6: not a sane save
    ld hl, ScrFull
    jr SramOff
:   ld c, a                 ; c = count = the new slot's index (a species
    ld b, 0                 ; already in the party or Pokédex may be added again)
    add hl, bc              ; hl = species[c]: species, $FF terminator; count + 1
    ld a, [wSpecies]
    ld [hli], a
    ld [hl], $FF
    ldptr hl, wParty
    ld a, c
    inc a
    ld [hl], a
    ; struct, OT name, nickname into slot c
    ld a, [wMon]
    ld [wMonSrc], a
    ld a, [wMon + 1]
    ld [wMonSrc + 1], a
    ld de, 1 + PARTY_LENGTH + 1
    add hl, de              ; hl = structs
    ld a, [wLen]
    ld e, a                 ; de = struct length
    ld b, a
    call CopySlot
    ld a, PARTY_LENGTH
    call AddTimes           ; hl = OT names
    ld e, NAME_LENGTH
    ld b, e
    call CopySlot
    ld a, PARTY_LENGTH
    call AddTimes           ; hl = nicknames
    ld b, e
    call CopySlot
    ; Pokédex: owned + seen
    ld a, [wDexBit]
    ld b, a
    ld a, [wDexByte]
    ld e, a
    ld d, 0
    ldptr hl, wDex
    add hl, de
    ld a, [hl]
    or b
    ld [hl], a
    ld a, [wSeenOfs]
    ld e, a
    ld d, 0
    add hl, de
    ld a, [hl]
    or b
    ld [hl], a
    ; new checksum, then read back: checksum, count, species
    push bc
    call Sum
    ldptr hl, wCk
    ld a, [wGen1]
    or a
    ld a, e
    jr z, :+
    cpl
    ld [hl], a
    jr :++
:   ld [hli], a
    ld [hl], d
:   call CheckSum
    pop bc
    jp nz, WriteFail
    ldptr hl, wParty
    ld a, [hli]
    dec a
    cp c
    jp nz, WriteFail
    ld b, 0
    add hl, bc
    ld a, [wSpecies]
    cp [hl]
    jp nz, WriteFail
    jp Done

; b bytes [hl+] -> [de+]
CopyBytes:
    ld a, [hli]
    ld [de], a
    inc de
    dec b
    jr nz, CopyBytes
    ret

; hl = hl + c*de, then copy the next b bytes from [wMonSrc] (advanced) there.
; Keeps de and c; returns hl = the base it was given.
CopySlot:
    push hl
    push de
    ld a, c
    call AddTimes
    ld a, [wMonSrc]
    ld e, a
    ld a, [wMonSrc + 1]
    ld d, a
:   ld a, [de]
    inc de
    ld [hli], a
    dec b
    jr nz, :-
    ld a, e
    ld [wMonSrc], a
    ld a, d
    ld [wMonSrc + 1], a
    pop de
    pop hl
    ret

; hl += a*de
AddTimes:
    or a
    ret z
    add hl, de
    dec a
    jr AddTimes

; z if the stored checksum matches (8-bit complement for Gen 1, 16-bit LE)
CheckSum:
    call Sum
    ldptr hl, wCk
    ld a, [wGen1]
    or a
    ld a, e
    jr z, :+
    cpl
    cp [hl]
    ret
:   cp [hl]
    ret nz
    inc hl
    ld a, d
    cp [hl]
    ret

; de = byte sum of [wSumLen] bytes from [wSumFrom]
Sum:
    ldptr hl, wSumLen
    ld b, h
    ld c, l
    ldptr hl, wSumFrom
    ld de, 0
:   ld a, [hli]
    add e
    ld e, a
    jr nc, :+
    inc d
:   dec bc
    ld a, b
    or c
    jr nz, :--
    ret

WaitVBlank:
    xor a
    ldh [rIF], a
    halt                    ; IME=0: wakes on VBlank without jumping to a vector
    ret

; Screen at hl: dw text colour, then lines (DrawLines). Written with the LCD
; on: every VRAM byte waits for STAT mode 0/1 (mode 2 that may follow is still
; VRAM-accessible), palette in VBlank.
DrawScreen:
    push hl
    call WaitVBlank
    ld a, $86               ; BG palette 0, colour 3
    ldh [rBCPS], a
    pop hl
    ld a, [hli]
    ldh [rBCPD], a
    ld a, [hli]
    ldh [rBCPD], a
    ; clear the visible 20x18 cells
    ld de, $9800
    ld c, 18
.row
    ld b, 20
:   ld a, ' '
    call PutVram
    dec b
    jr nz, :-
    ld a, e
    add 32 - 20
    ld e, a
    jr nc, :+
    inc d
:   dec c
    jr nz, .row
    ; fall through
; Lines at hl: {row, col, ASCII..., 0}*, $FF
DrawLines:
    ld a, [hli]             ; row, or $FF
    cp $FF
    ret z
    swap a                  ; de = $9800 + row*32 + col
    ld e, a
    and $0F
    ld d, a
    ld a, e
    and $F0
    add a
    ld e, a
    rl d
    ld a, [hli]
    add e
    ld e, a
    ld a, d
    adc HIGH($9800)
    ld d, a
:   ld a, [hli]
    or a
    jr z, DrawLines
    call PutVram
    jr :-

; [de+] = a, once VRAM is accessible
PutVram:
    push af
:   ldh a, [rSTAT]
    and 2
    jr nz, :-
    pop af
    ld [de], a
    inc de
    ret

CrystalTitle:
    db "PM_CRYSTAL", 0, "BYT", 0 ; $0134 title, $013F game code ($0142 = language)
GSTitle:
    db "POKEMON_", 0        ; GLD / SLV, game code AAU? / AAX?
Gen1Title:
    db "POKEMON ", 0        ; RED / BLUE / YELLOW (+ localised names); the MBC
                            ; type then excludes the other "POKEMON " carts
Languages:                  ; supported Gen-2 releases
    db "EDFIS", 0           ; EN (US/EU), DE, FR, IT, ES

; AddMon parameters, copied to wParams (same field order)
MACRO layout                ; party, struct len, dex owned, seen - owned,
    dw \1                   ; sum from, sum end, checksum at, gen1?
    db \2
    dw \3
    db \4
    dw \5, \6 - \5, \7
    db \8
ENDM
MACRO mon                   ; dex no, data, species
    db (\1 - 1) / 8, 1 << ((\1 - 1) % 8)
    dw \2
    db \3
ENDM
LayoutRBY:
    layout $AF2C, 44, $A5A3, 19, $A598, $B523, $B523, 1
    ASSERT @ - LayoutRBY == wMonParams - wParams
LayoutGS:
    layout $A88A, 48, $AA4C, 32, $A009, $AD69, $AD69, 0
LayoutC:
    layout $A865, 48, $AA27, 32, $A009, $AB83, $AD0D, 0
MonMew:
    mon MEW_DEX, MewData, MEW_SPECIES
    ASSERT @ - MonMew == wParamsEnd - wMonParams
MonMew2:
    mon MEW2_DEX, Mew2Data, MEW2_SPECIES
MonCelebi:
    mon CELEBI_DEX, CelebiData, CELEBI_SPECIES

MewData:
    MEW_DATA                ; party struct, OT name, nickname (mew.inc)
Mew2Data:
    MEW2_DATA               ; the same Mew after the Time Capsule (mew2.inc)
CelebiData:
    CELEBI_DATA             ; (celebi.inc)

MACRO screen                ; colour (BGR555)
    dw \1
ENDM
MACRO text                  ; row, col, "text"
    db \1, \2, \3, 0
ENDM
DEF WHITE  EQU $7FFF
DEF GREEN  EQU $23E8
DEF YELLOW EQU $03FF
DEF RED    EQU $1CFF

ScrPrompt:
    screen WHITE
    text 1, 0, "SAVE PATCHER"
    text 5, 0, "INSERT GAME,"
    text 6, 0, "PRESS START"
    db $FF
ScrMenu:
    screen WHITE
    text 11, 0, "SELECT: BACK"
    db $FF
ActC:
    text 9, 0, "START: GS BALL EVENT"
ActGS:
    text 7, 0, "B: ADD {CELEBI_NAME}"
ActRBY:
    text 5, 0, "A: ADD {MEW_NAME}"
    db $FF
ScrDone:
    screen GREEN
    text 5, 0, "DONE."
    db $FF
ScrAlready:
    screen YELLOW
    text 5, 0, "ALREADY DONE"
    db $FF
ScrFull:
    screen YELLOW
    text 5, 0, "PARTY FULL"
    db $FF
ScrBadCart:
    screen RED
    text 5, 0, "NOT SUPPORTED"
    db $FF
ScrNoSave:
    screen RED
    text 5, 0, "NO SAVE FOUND"
    db $FF
ScrWriteFail:
    screen RED
    text 5, 0, "WRITE FAILED"
    db $FF

IF DEF(STANDALONE)
; The loader's menu font (host/gbcpop.c font5x7): 5x7 glyphs for ASCII
; $2C..$5F, one 5-bit row value per line. Tiles $20..$2B stay blank (space).
Font:
    db $00,$00,$00,$00,$0C,$04,$08 ; ,
    db $00,$00,$00,$1F,$00,$00,$00 ; -
    db $00,$00,$00,$00,$00,$0C,$0C ; .
    db $00,$01,$02,$04,$08,$10,$00 ; /
    db $0E,$11,$13,$15,$19,$11,$0E ; 0
    db $04,$0C,$04,$04,$04,$04,$0E ; 1
    db $0E,$11,$01,$02,$04,$08,$1F ; 2
    db $1F,$02,$04,$02,$01,$11,$0E ; 3
    db $02,$06,$0A,$12,$1F,$02,$02 ; 4
    db $1F,$10,$1E,$01,$01,$11,$0E ; 5
    db $06,$08,$10,$1E,$11,$11,$0E ; 6
    db $1F,$01,$02,$04,$08,$08,$08 ; 7
    db $0E,$11,$11,$0E,$11,$11,$0E ; 8
    db $0E,$11,$11,$0F,$01,$02,$0C ; 9
    db $00,$0C,$0C,$00,$0C,$0C,$00 ; :
    db $00,$0C,$0C,$00,$0C,$04,$08 ; ;
    db $02,$04,$08,$10,$08,$04,$02 ; <
    db $00,$00,$1F,$00,$1F,$00,$00 ; =
    db $08,$04,$02,$01,$02,$04,$08 ; >
    db $0E,$11,$01,$02,$04,$00,$04 ; ?
    db $0E,$11,$01,$0D,$15,$15,$0E ; @
    db $0E,$11,$11,$11,$1F,$11,$11 ; A
    db $1E,$11,$11,$1E,$11,$11,$1E ; B
    db $0E,$11,$10,$10,$10,$11,$0E ; C
    db $1C,$12,$11,$11,$11,$12,$1C ; D
    db $1F,$10,$10,$1E,$10,$10,$1F ; E
    db $1F,$10,$10,$1E,$10,$10,$10 ; F
    db $0E,$11,$10,$17,$11,$11,$0F ; G
    db $11,$11,$11,$1F,$11,$11,$11 ; H
    db $0E,$04,$04,$04,$04,$04,$0E ; I
    db $07,$02,$02,$02,$02,$12,$0C ; J
    db $11,$12,$14,$18,$14,$12,$11 ; K
    db $10,$10,$10,$10,$10,$10,$1F ; L
    db $11,$1B,$15,$15,$11,$11,$11 ; M
    db $11,$11,$19,$15,$13,$11,$11 ; N
    db $0E,$11,$11,$11,$11,$11,$0E ; O
    db $1E,$11,$11,$1E,$10,$10,$10 ; P
    db $0E,$11,$11,$11,$15,$12,$0D ; Q
    db $1E,$11,$11,$1E,$14,$12,$11 ; R
    db $0F,$10,$10,$0E,$01,$01,$1E ; S
    db $1F,$04,$04,$04,$04,$04,$04 ; T
    db $11,$11,$11,$11,$11,$11,$0E ; U
    db $11,$11,$11,$11,$11,$0A,$04 ; V
    db $11,$11,$11,$15,$15,$15,$0A ; W
    db $11,$11,$0A,$04,$0A,$11,$11 ; X
    db $11,$11,$11,$0A,$04,$04,$04 ; Y
    db $1F,$01,$02,$04,$08,$10,$1F ; Z
    db $0E,$08,$08,$08,$08,$08,$0E ; [
    db $00,$10,$08,$04,$02,$01,$00 ; backslash
    db $0E,$02,$02,$02,$02,$02,$0E ; ]
    db $04,$0A,$11,$00,$00,$00,$00 ; ^
    db $00,$00,$00,$00,$00,$00,$1F ; _
FontEnd:
    ASSERT FontEnd - Font == ($60 - FONT_FIRST) * 7
ENDC
PayloadEnd:
ENDL

SECTION "vars", WRAM0[$CE00]  ; below the stack ($D000 down)
wGame:    ds 1              ; GAME_* of the cartridge the menu is for
wMonSrc:  ds 2              ; CopySlot's running source pointer
wParams:                    ; AddMon's Layout block (see `layout`)
wParty:   ds 2              ; party count address
wLen:     ds 1              ; party struct length
wDex:     ds 2              ; Pokédex owned array
wSeenOfs: ds 1              ; seen array - owned array
wSumFrom: ds 2              ; checksummed range
wSumLen:  ds 2
wCk:      ds 2              ; checksum address
wGen1:    ds 1              ; 1: 8-bit complement checksum, 0: 16-bit LE
wMonParams:                 ; ... and Mon block (see `mon`)
wDexByte: ds 1              ; the species' byte and bit in the Pokédex arrays
wDexBit:  ds 1
wMon:     ds 2              ; struct + OT name + nickname
wSpecies: ds 1
wParamsEnd:
