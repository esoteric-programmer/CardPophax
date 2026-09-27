; payload.asm — resident infrared bridge for gbcpop
;
; Uploaded into the Pokémon TCG cartridge's work RAM with RPC command 3 and
; entered with RPC command 4. From that moment the Game Boy executes nothing
; but this code: it carries its own IR bit-banger, so the cartridge can be
; pulled out and replaced while the link to the Raspberry Pi stays up.
;
; The bit layer is byte-for-byte the game's own (06:$56DF..$57FC), so the
; timing is identical to what the Pi is already talking to.
;
; Build:  rgbasm -o payload.o payload.asm
;         rgblink -o payload.gb payload.o && head -c $SIZE payload.gb > payload.bin
; (the Makefile does this and trims to the real length)

DEF PAYLOAD_ORG EQU $C700          ; dead scratch in the TCG's WRAM
DEF STACK_TOP EQU $CFFE

DEF rP1 EQU $FF00
DEF rRP EQU $FF56
DEF rIF EQU $FF0F
DEF rIE EQU $FFFF

DEF MBC_RAM_ENABLE EQU $0000
DEF MBC_ROM_BANK EQU $2000
DEF MBC_RAM_BANK EQU $4000

SECTION "payload image", ROM0[$0000]
LOAD "payload", WRAM0[PAYLOAD_ORG]

; ---------------------------------------------------------------- entry ---

Start::
	di
	xor a
	ld [rIE], a            ; no interrupt vector can pull us back into ROM
	ldh [rIF], a
	ld sp, STACK_TOP
	ld a, $C0              ; IR receiver enabled, LED off
	ldh [rRP], a

MainLoop:
	call SyncAsReceiver
	call RecvCommand
	jr c, MainLoop
	ld a, [CmdOp]
	cp NUM_OPS
	jr nc, MainLoop
	add a
	ld e, a
	ld d, $00
	ld hl, OpTable
	add hl, de
	ld a, [hl+]
	ld h, [hl]
	ld l, a
	jp hl

OpTable:
	dw OpPing              ; 0
	dw OpRead              ; 1
	dw OpWrite             ; 2
	dw OpMapper            ; 3
	dw OpHalt              ; 4
DEF NUM_OPS EQU 5
DEF PAYLOAD_IMAGE EQU 512        ; bytes the Pi uploads; must be >= the code

; ------------------------------------------------------------- handlers ---

; reply: one byte $4B
OpPing:
	ld a, $4B
	ld [Scratch], a
	call ReplyDelay
	ld hl, Scratch
	ld c, $01
	call SendBlock
	jp MainLoop

; CmdBank -> RAM bank ($FF = leave the mapper alone)
; CmdAddr, CmdLen -> source, 0 = 256 bytes
OpRead:
	call SelectBank
	call ReplyDelay
	call GetAddr
	ld a, [CmdLen]
	ld c, a
	call SendBlock
	jp MainLoop

OpWrite:
	call SelectBank
	call GetAddr
	ld a, [CmdLen]
	ld c, a
	call RecvBlock
	jr c, MainLoop         ; bad checksum: stay silent, the Pi retries
	xor a
	ld [Scratch], a
	call ReplyDelay
	ld hl, Scratch
	ld c, $01
	call SendBlock
	jp MainLoop

; raw mapper write: CmdBank -> [CmdAddr]. Use for RAM enable ($0000 = $0A),
; RAM bank ($4000) and ROM bank ($2000) on the cartridge currently inserted.
OpMapper:
	call GetAddr
	ld a, [CmdBank]
	ld [hl], a
	ld a, $4B
	ld [Scratch], a
	call ReplyDelay
	ld hl, Scratch
	ld c, $01
	call SendBlock
	jp MainLoop

; park forever; end the session by power-cycling the console
OpHalt:
	di
.spin
	jr .spin

; ------------------------------------------------------------- helpers ---

SelectBank:
	ld a, [CmdBank]
	inc a
	ret z                  ; $FF: do not touch the mapper
	dec a
	ld [MBC_RAM_BANK], a
	ret

GetAddr:
	ld a, [CmdAddrLo]
	ld l, a
	ld a, [CmdAddrHi]
	ld h, a
	ret

; ~1.7 ms, so the Pi is always listening before we answer
ReplyDelay:
	push de
	ld de, $0100
.loop
	dec de
	ld a, d
	or e
	jr nz, .loop
	pop de
	ret

; ------------------------------------------------------------ IR: send ---

; carry = bit to send (a pulse means 0), hl = rRP.
; Both paths are padded to exactly 384 T so the cell length is data-independent.
SendBit:
	jr c, .one
	ld [hl], $C1           ; LED on
	ld a, $05
	jr .d1
.d1
	dec a
	jr nz, .d1
	ld [hl], $C0           ; LED off, 108 T later
	ld a, $0E
	jr .d2
.d2
	dec a
	jr nz, .d2
	ret
.one
	ld a, $15
	jr .d3
.d3
	dec a
	jr nz, .d3
	nop
	ret

; a = byte. One idle cell, one start pulse, then 8 data bits LSB first.
SendByte:
	push hl
	push de
	push bc
	ld hl, rRP
	ld b, a
	scf
	call SendBit
	or a
	call SendBit
	ld c, $08
	ld c, $08
.loop
	ld a, $00
	rr b
	call SendBit
	dec c
	jr nz, .loop
	pop bc
	pop de
	pop hl
	ret

; hl = source, c = count (0 = 256). Sends the payload then the byte that
; makes the whole block sum to 0 mod 256.
SendBlock:
	ld b, $00
.loop
	ld a, b
	add [hl]
	ld b, a
	ld a, [hl+]
	call SendByte
	dec c
	jr nz, .loop
	ld a, b
	cpl
	inc a
	jp SendByte

; ------------------------------------------------------------ IR: recv ---

; returns a = byte, carry clear. Carry set = no start pulse within ~2.2 ms.
RecvByte:
	push de
	push bc
	push hl
	ld b, $00
	ld hl, rRP
.wait
	bit 1, [hl]            ; 0 = light is being received
	jr z, .got
	dec b
	jr nz, .wait
	pop hl
	pop bc
	pop de
	scf
	ld a, $FF
	ret
.got
	ld a, $0F
.settle
	dec a
	jr nz, .settle
	ld e, $08
.cell
	ld a, $01
	ld b, $09
	ld b, $09
	ld b, $09
	ld b, $09
	bit 1, [hl]
	jr nz, .sample
	xor a
.sample
	bit 1, [hl]
	jr nz, .next
	xor a
.next
	dec b
	jr nz, .sample
	rrca
	rr d
	dec e
	jr nz, .cell
	ld a, d
	pop hl
	pop bc
	pop de
	or a
	ret

; RecvByte, but waits ~70 ms for the start pulse instead of 2.2 ms
RecvByteWait:
	push bc
	ld c, $20
.retry
	call RecvByte
	jr nc, .done
	dec c
	jr nz, .retry
	scf
.done
	pop bc
	ret

; hl = destination, c = count (0 = 256). Carry set on timeout or bad checksum.
RecvBlock:
	ld b, $00
.loop
	call RecvByte
	jr c, .fail
	ld [hl+], a
	add b
	ld b, a
	dec c
	jr nz, .loop
	call RecvByte
	jr c, .fail
	add b
	or a
	ret z
.fail
	scf
	ret

; wait for $AA (indefinitely), answer $33
SyncAsReceiver:
.wait
	call RecvByte
	jr c, .wait
	cp $AA
	jr nz, .wait
	ld a, $33
	jp SendByte

; six-byte command block plus checksum, with a long wait on the first byte
RecvCommand:
	call RecvByteWait
	ret c
	ld hl, CmdOp
	ld [hl+], a
	ld b, a
	ld c, $05
.loop
	call RecvByte
	ret c
	ld [hl+], a
	add b
	ld b, a
	dec c
	jr nz, .loop
	call RecvByte
	ret c
	add b
	or a
	ret z
	scf
	ret

; ----------------------------------------------------------------- data ---

CmdOp:     db 0             ; 0 ping, 1 read, 2 write, 3 mapper write, 4 halt
CmdBank:   db 0             ; RAM bank, or the byte for a mapper write
CmdAddrLo: db 0
CmdAddrHi: db 0
CmdLen:    db 0             ; 0 = 256
CmdSpare:  db 0
Scratch:   db 0

PayloadCodeEnd::
	ds PAYLOAD_IMAGE - (PayloadCodeEnd - Start)   ; pad to a round upload size
PayloadEnd::

ENDL

DEF PAYLOAD_SIZE EQU PayloadEnd - Start
	PRINTLN "payload size: ", PAYLOAD_SIZE, " bytes, ", PAYLOAD_ORG, "..", PAYLOAD_ORG + PAYLOAD_SIZE - 1
