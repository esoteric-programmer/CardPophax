; ------------------------------------------------------------------------
; irbit.asm — GBC IR mark/space bit layer for the homebrew loader (SM83)
;
; Self-clocking: a bit = a short MARK (LED on) then a SPACE (LED off); the
; SPACE length encodes the value (short=0, long=1). RX measures mark-to-mark
; gaps and splits on RX_THRESH. Reuses the Generation-2 Mystery Gift scheme,
; which is proven on this exact LED/op-amp/receiver hardware.
;
; Timing constants live in hardware.inc; how they are sized (poll resolution,
; margins, the lead budget) is in docs/05-loader-protocol.md §2.
;
; Public routines (all preserve nothing unless noted):
;   ir_send_byte   a = byte -> transmit (MSB first)
;   ir_recv_byte   -> a = byte, carry set on timeout
; ------------------------------------------------------------------------

; delay: b inner-loop iterations (~16 cycles each at single speed). Clobbers a,b.
ir_delay:
	dec b
	jr nz, ir_delay
	ret

; --- transmit one byte, MSB first ---
; a = byte
ir_send_byte:
	push bc
	push de
	ld e, a               ; byte to send
	ld d, 8               ; bits
.bit
	ld a, IR_ON           ; mark
	ldh [rRP], a
	ld b, TX_MARK
	call ir_delay
	ld a, IR_OFF          ; space starts
	ldh [rRP], a
	sla e                 ; MSB -> carry
	jr c, .one
	ld b, TX_SPACE0
	jr .space
.one
	ld b, TX_SPACE1
.space
	call ir_delay
	dec d
	jr nz, .bit
	; trailing mark so the last bit's space is bounded, then idle
	ld a, IR_ON
	ldh [rRP], a
	ld b, TX_MARK
	call ir_delay
	ld a, IR_OFF
	ldh [rRP], a
	pop de
	pop bc
	ret

; --- receive one byte, self-clocking ---
; returns a = byte; carry set = timed out (treat as end-of-frame / error)
ir_recv_byte:
	push bc                   ; preserve caller's bc (loop counters etc.):
	push de                   ; this routine and ir_measure_space clobber b,c
	; --- frame sync: a run of idle (the inter-byte gap) then bit7's mark ---
	ld bc, RX_SYNC_BUDGET     ; poll budget -> quick timeout on a drop
	ld e, 0                   ; consecutive-idle counter
.sync
	ldh a, [rRP]
	bit 1, a
	jr nz, .idle              ; bit1=1 -> idle (light off)
	ld a, e                   ; light on (a mark)
	cp FRAME_IDLE
	jr nc, .synced            ; enough idle before it -> byte start
	ld e, 0                   ; mid-stream mark: reset the idle run
	jr .tick
.idle
	inc e
.tick
	dec bc
	ld a, b
	or c
	jr nz, .sync
	pop de                    ; sync timeout
	pop bc
	scf
	ret
.synced
	; at bit7's mark; measure 8 spaces (ir_measure_space clobbers a,b,c)
	ld d, 0                   ; result
	ld e, 8                   ; bit count
.b
	call ir_measure_space
	jr c, .to
	cp RX_THRESH
	ccf
	rl d
	dec e
	jr nz, .b
	ld a, d
	pop de
	pop bc
	or a                      ; clear carry = success
	ret
.to
	pop de
	pop bc
	scf
	ret

; from inside a mark, wait for light-off then count until the next light-on.
; returns a = space count; carry set on timeout. clobbers a,b,c.
ir_measure_space:
	ld b, RX_TIMEOUT      ; bound the mark
.woff
	ldh a, [rRP]
	bit 1, a
	jr nz, .off           ; bit1=1 -> light off
	dec b
	jr nz, .woff
	scf
	ret
.off
	ld c, 0               ; space counter
.won
	ldh a, [rRP]
	bit 1, a
	jr z, .on             ; light on -> next mark reached
	inc c
	ld a, c
	cp RX_TIMEOUT
	jr nc, .tmo           ; no next mark in budget -> end/timeout
	jr .won
.on
	ld a, c
	or a
	ret
.tmo
	scf
	ret
