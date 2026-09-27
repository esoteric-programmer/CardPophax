; Card Pop! gift for the Pokémon Trading Card Game — runs entirely from
; WRAM/HRAM. Adds a Mew (A) or a Venusaur (B) to the TCG save the way a
; successful Card Pop! does, so the save looks like one more real Card Pop!:
;
;   * sTotalCardPopsDone ($A005) + 1, and the partner's name into
;     sCardPopNameList ($BB00, 16 x 16 bytes) at slot (old count & 15)
;   * the card into sCardCollection ($A100 + id): count (collection + the four
;     built decks) must be < 99; count + 1, "not owned" bit 7 cleared
;   * album progress ($B8FE: owned ids, NUM_CARDS minus unowned Venusaur/Mew)
;   * both of these in SRAM bank 2 (the backup) first, then bank 0, exactly as
;     _AddCardToCollectionAndUpdateAlbumProgress does
;
; pret/poketcg: engine/link/card_pop.asm, engine/save.asm,
; home/card_collection.asm, sram.asm. None of these bytes are covered by the
; general save data checksum ($B804 over $B808.., which ends at $B8BB).
;
; The partner is always "ATTINY" (6 full-width letters, $03 xx, then $00 00),
; followed by the two bytes the name entry fills from the RNG. All 65536 byte
; pairs are reachable by the game's RNG, so any pair is a plausible name:
;   the pair makes sum(player name) - sum(partner) = 5 (mod 256), the rule
;   by which DecideCardToReceiveFromCardPop takes its rare branch (16-byte
;   byte sums). That branch gives Venusaur if bit 0 of the XOR difference is
;   0, else Mew Lv15; bit 0 of the XOR always equals bit 0 of the sum, so it
;   is always Mew. For Mew this is a pop the real game would have made; for
;   Venusaur (never obtainable through a real Card Pop!) it is the nearest
;   possible: the rare branch, with only the unreachable parity bit off.
; The pair is chosen so the name is not in the list yet (a real Card Pop!
; with an already listed partner is refused).
;
; Flow: one menu, A: Mew / B: Venusaur (the TCG is inserted already: no swap,
; no prompt). The button checks the cartridge, adds the card or shows why not
; for ~2.5 s, then the menu again. SRAM is only enabled while patching. The TCG
; itself was overwritten in WRAM: reset the Game Boy to play it. Launched by the
; homebrew loader, SELECT returns to its menu (not shown on screen).
;
; Build: make card-pop.bin (the $C000 payload) / make (card-pop.gbc,
;        standalone) / make tests && ./test_card_pop.py

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


; MBC5
DEF MBC_RAMG  EQU $0000     ; $0A enables SRAM, $00 disables it
DEF MBC_RAMB  EQU $4000     ; SRAM bank 0-3

; pret/poketcg sram.asm (bank 0; the collection, decks and album progress are
; also backed up in bank 2)
DEF sTotalCardPopsDone EQU $A005
DEF sPlayerName        EQU $A010
DEF sCardCollection    EQU $A100 ; 256 bytes, count + CARD_NOT_OWNED (bit 7)
DEF sDeck1             EQU $A200 ; 4 built decks: 24-byte name, 60 card ids
DEF DECK_STRUCT_SIZE   EQU $54
DEF DECK_NAME_SIZE     EQU 24
DEF DECK_SIZE          EQU 60
DEF sGeneralSaveData   EQU $B800 ; $08 $00, byte count, checksum, data
DEF sAlbumProgress     EQU $B8FE ; owned, to collect
DEF sCardPopNameList   EQU $BB00
DEF NAME_BUFFER_LENGTH EQU 16
DEF NUM_CARDS          EQU 228
DEF MAX_AMOUNT_OF_CARD EQU 99
DEF VENUSAUR_LV64      EQU $0A
DEF MEW_LV15           EQU $A1

; joypad, buttons half (P1 = $10)
DEF PAD_A      EQU 1
DEF PAD_B      EQU 2
DEF PAD_SELECT EQU 4

DEF FONT_FIRST  EQU $2C     ; tile index == ASCII code; ' ' is a blank tile
DEF SHOW_FRAMES EQU 150     ; result screen: 2.5 s at ~60 Hz
DEF BG_COLOR    EQU $2842   ; BGR555 dark blue
DEF rDIV EQU $FF04

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
    ldh [rNR52], a          ; APU off (the TCG may be swapped for another)
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

Menu:
    ld hl, ScrMenu
    call DrawScreen
IF DEF(STANDALONE)
    ld c, PAD_A | PAD_B
ELSE
    ld c, PAD_A | PAD_B | PAD_SELECT
ENDC
    call WaitButton         ; -> a = the pressed button(s) of c
IF !DEF(STANDALONE)
    cp PAD_SELECT           ; SELECT: back to the loader's menu
    jr z, ToLoader
ENDC
    ld b, a
    push bc
    call Identify           ; z if not a supported TCG cartridge
    pop bc
    ld hl, ScrBadCart
    jr z, Show
    ld a, MEW_LV15          ; A: Mew
    bit 1, b                ; PAD_B: Venusaur
    jr z, :+
    ld a, VENUSAUR_LV64
:   ld [wCard], a
    call CardPop
Show:                       ; hl = result screen, ~2.5 s, then the menu
    call DrawScreen
    ld c, SHOW_FRAMES
:   call WaitVBlank
    dec c
    jr nz, :-
    jr Menu

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

; nz if the cartridge is the Western TCG: header checksum ($014D), destination
; 1 (not Japanese), title "POKECARD", MBC5+RAM+BATTERY, 32 KB SRAM
Identify:
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
    dec a
    jr nz, .none
    ld a, [$0149]
    cp $03
    jr nz, .none
    ld a, [$0147]
    cp $1B
    jr nz, .none
    ld de, TcgTitle
    call CheckTitle
    jr nz, .none
    or 1
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

; One simulated Card Pop! of [wCard]. Returns hl = result screen.
CardPop:
    ld a, $0A
    ld [MBC_RAMG], a
    xor a
    ld [MBC_RAMB], a
    ; a save must exist: general save data header $08 $00 and its checksum
    ld hl, sGeneralSaveData
    ld a, [hli]
    cp $08
    jp nz, .nosave
    ld a, [hli]
    or a
    jp nz, .nosave
    ld a, [hli]             ; byte count
    ld c, a
    ld a, [hli]
    ld b, a
    ld hl, sGeneralSaveData + 8
    call Sum
    ld hl, sGeneralSaveData + 4
    ld a, e
    cp [hl]
    jp nz, .nosave
    inc hl
    ld a, d
    cp [hl]
    jp nz, .nosave
    ld a, [sPlayerName]
    or a
    jp z, .nosave
    ; room for one more card (bank 0; same rule as AddCardToCollection)
    call CardTotal          ; -> a, hl = the collection entry
    cp MAX_AMOUNT_OF_CARD
    jp nc, .limit
    ld a, [hl]
    and $7F
    ld [wOld], a

    ; partner name: "ATTINY" + two bytes that make it new (and, for Mew, legit)
    ld hl, Attiny
    ld de, wName
    ld b, NAME_BUFFER_LENGTH - 2
:   ld a, [hli]
    ld [de], a
    inc de
    dec b
    jr nz, :-
    ld hl, Attiny
    ld bc, NAME_BUFFER_LENGTH - 2
    call Sum
    ld a, e
    ld [wTarget], a         ; sum("ATTINY" + $00 $00)
    ld hl, sPlayerName
    ld bc, NAME_BUFFER_LENGTH
    call Sum                ; e = sum(player name)
    ld a, [wTarget]
    ld b, a
    ld a, e
    sub 5
    sub b
    ld [wTarget], a         ; r1 + r2 = sum(player) - 5 - sum("ATTINY")
    ldh a, [rDIV]
    ld [wName + 14], a      ; r1: a random start
.try
    ld a, [wName + 14]      ; r2 = target - r1 (both cards, see the top)
    ld b, a
    ld a, [wTarget]
    sub b
    ld [wName + 15], a
    call InList
    jr nz, .unique
    ld hl, wName + 14       ; listed already: next r1
    inc [hl]
    jr .try

.unique
    ; sTotalCardPopsDone + 1; name into slot (old count & 15)
    ld hl, sTotalCardPopsDone
    ld a, [hl]
    ld [wCount], a
    inc [hl]
    call NameSlot
    ld de, wName
    ld b, NAME_BUFFER_LENGTH
:   ld a, [de]
    inc de
    ld [hli], a
    dec b
    jr nz, :-
    ; the card: backup bank 2 first, then bank 0, as the game does
    ld a, 2
    ld [MBC_RAMB], a
    call AddCard
    xor a
    ld [MBC_RAMB], a
    call AddCard

    ; read back (bank 0): counter, name, card count
    ld a, [wCount]
    inc a
    ld hl, sTotalCardPopsDone
    cp [hl]
    jr nz, .fail
    call NameSlot
    ld de, wName
    ld b, NAME_BUFFER_LENGTH
:   ld a, [de]
    inc de
    cp [hl]
    jr nz, .fail
    inc hl
    dec b
    jr nz, :-
    ld a, [wCard]
    ld l, a
    ld h, HIGH(sCardCollection)
    ld a, [wOld]
    inc a
    cp [hl]                 ; count + 1, "not owned" bit clear
    jr nz, .fail
    ld hl, ScrDone
    jr SramOff
.fail
    ld hl, ScrWriteFail
    jr SramOff
.limit
    ld hl, ScrLimit
    jr SramOff
.nosave
    ld hl, ScrNoSave
    ; fall through
SramOff:
    xor a
    ld [MBC_RAMG], a
    ret

; hl = sCardPopNameList + (wCount & 15) * 16
NameSlot:
    ld a, [wCount]
    and $0F
    swap a
    ld l, a
    ld h, HIGH(sCardPopNameList)
    ret

; z if wName (16 bytes) is in sCardPopNameList
InList:
    ld hl, sCardPopNameList
.entry
    ld de, wName
    ld b, NAME_BUFFER_LENGTH
    ld c, 0                 ; differences
:   ld a, [de]
    inc de
    cp [hl]
    jr z, :+
    inc c
:   inc hl
    dec b
    jr nz, :--
    ld a, c
    or a
    ret z                   ; all 16 bytes equal
    ld a, l
    or a                    ; after the 16th entry hl = $BC00
    jr nz, .entry
    inc a                   ; nz: not listed
    ret

; AddCardToCollection + UpdateAlbumProgress for [wCard], current SRAM bank
AddCard:
    call CardTotal
    cp MAX_AMOUNT_OF_CARD
    ret nc
    ld a, [hl]
    and $7F
    inc a
    ld [hl], a
    ; album progress: owned ids, and NUM_CARDS less unowned Venusaur / Mew
    ld hl, sCardCollection
    ld d, 0
:   bit 7, [hl]
    jr nz, :+
    inc d
:   inc l
    jr nz, :--
    ld e, NUM_CARDS
    ld a, [sCardCollection + VENUSAUR_LV64]
    add a
    jr nc, :+
    dec e
:   ld a, [sCardCollection + MEW_LV15]
    add a
    jr nc, :+
    dec e
:   ld hl, sAlbumProgress
    ld a, d
    ld [hli], a
    ld [hl], e
    ret

; a = copies of [wCard] owned: collection count + the four built decks
; (CreateTempCardCollection); hl = the collection entry. Current SRAM bank.
CardTotal:
    ld hl, sDeck1
    ld c, 4
    ld b, 0
.deck
    ld a, [hl]
    or a                    ; empty name: no deck
    jr z, .next
    push hl
    ld de, DECK_NAME_SIZE
    add hl, de
    ld a, [wCard]
    ld d, a
    ld e, DECK_SIZE
:   ld a, [hli]
    cp d
    jr nz, :+
    inc b
:   dec e
    jr nz, :--
    pop hl
.next
    ld de, DECK_STRUCT_SIZE
    add hl, de
    dec c
    jr nz, .deck
    ld a, [wCard]
    ld l, a
    ld h, HIGH(sCardCollection)
    ld a, [hl]
    and $7F
    add b
    ret

; de = 16-bit byte sum of bc bytes from hl
Sum:
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

TcgTitle:
    db "POKECARD", 0        ; $0134 title (US AXQE; the European release too)

Attiny:                     ; the partner's name: "ATTINY" in the full-width
    db $03, $30, $03, $43, $03, $43 ; set ($03 xx, A = $30), then TX_END $00 $00;
    db $03, $38, $03, $3D, $03, $48 ; bytes 14-15 (the RNG pair) are chosen at
    db $00, $00                     ; run time

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

ScrMenu:
    screen WHITE
    text 1, 0, "CARD POP GIFT"
    text 5, 0, "A: ADD MEW"
    text 7, 0, "B: ADD VENUSAUR"
    db $FF
ScrDone:
    screen GREEN
    text 5, 0, "DONE. RESET THE"
    text 6, 0, "GAME BOY TO PLAY."
    db $FF
ScrLimit:
    screen YELLOW
    text 5, 0, "99 CARDS ALREADY"
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
wCard:   ds 1               ; card id to add
wOld:    ds 1               ; its collection count before (bank 0)
wCount:  ds 1               ; sTotalCardPopsDone before
wTarget: ds 1               ; Mew: r1 + r2
wName:   ds NAME_BUFFER_LENGTH ; the partner's name
