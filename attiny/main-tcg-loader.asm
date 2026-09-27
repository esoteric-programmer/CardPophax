;
; TCG Card Pop! -- ATtiny85  HOMEBREW LOADER LAUNCHER (standalone gbcpop `loader`)
;
; The GBC sits on the Card Pop! screen as slave (do NOT press A there; it runs
; IR_ServeLoop). The ATtiny, as master, does what `gbcpop loader` does on VBA:
;   Stage 0  Card Pop! RPC: cmd 3 writes the bootstrap into WRAM $C000 (128-byte
;            chunks), cmd 4 calls it.
;   Stage 1  mark/space link (docs/05-loader-protocol.md): the loader,
;            the menu font and the menu text, each chunked and ACKed.
;            The GBC shows the menu.
;   Stage 2  on a REQ byte (payload id), send that payload's manifest and
;            body frames. The loader relocates and jumps.
; The idle loop keeps no state about the GBC: each pass probes for the TCG's
; Card Pop! screen ($AA -> $33, then Stages 0-1) and, if nothing answers,
; listens ~400 ms for a REQ from the loader's menu (Stage 2). So the GBC may be
; power-cycled, this chip reset, or a payload may return to the menu (SELECT)
; at any time. A lost REQ or a failed transfer: the loader sends the REQ once
; more by itself, then goes back to its menu.
; All frames are precomputed on the PC into loader_data.inc:
;   host$ ./gbcpop attiny-inc ../attiny/loader_data.inc \
;       ../gb/bootstrap.bin ../gb/loader.bin <payload1> [<payload2> [<payload3>]]
;   (a payload = a .bin entered at $C000, or a manifest.txt, docs/07)
;
; Link timing: the GBC runs our code at CGB normal speed (cmd 4 executes inside
; the TCG's single-speed IR section), so gbcpop's MS_* values are real us here:
; mark 48, space 64 / 224, lead 800 (8 us timer ticks below). The GB's REQ
; bits are ~190 us ('0') / ~490 us ('1') mark-to-mark. MS_LEAD, the GB's
; FRAME_IDLE and gbcpop's RLE_MAX_RUN are sized together (docs/05 §2).
;
; Every stream goes out in chunks (<= 128 raw bytes, one frame each:
; $55 $55 START ID SEQ len16 body CK16); each chunk is sent up to TRIES times
; until the GB ACKs it with the frame's CK_lo (stop-and-wait per chunk).
;
; LEDs: power-on 1 blue blink. Blue = sending and the GBC answers (Card Pop!
; sync, chunk ACKs); red = the last chunk copy got no ACK (until one does); both
; off = searching / listening, or done. They are dark during each ACK window
; (the GBC's IR sensor picks them up), which is not visible. A failed round
; (after the GBC had answered) = 3 red blinks, retry after a pause.
;
; DEBUG builds only (make DEBUG=1) keep a trace in EEPROM (avrdude ... -U
; eeprom:r:ee.bin:r): [0]=PROGRESS (bootstrap chunks written) [1]=SUBSTEP: 10
; bootstrap called, 20 loader+menu sent, 40 payload sent
; [2]=TRACE_PTR [3]=TRACE_WRAPPED [4..]=TRACE [tag,val]: 0xAC val = write ack
; (a wrong one is retried, WRITE_TRIES), 0x5F 00 = sync failed mid-round,
; 0xE0 id = REQ byte received, 0xC1..0xC8 b = reply heard after copy 1..8
; (FF = none), 0xCF id = stream id failed. The trace starts at a round's
; first Card Pop! sync. EEPROM 0x000: the last round or payload that failed;
; 0x100: the last payload sent.
;
; !!! FUSES unchanged (lfuse 0x62 hfuse 0xdf efuse 0xff); no external clock. !!!
; BUILD: make [DEBUG=1]  (avra [-D DEBUG] main-tcg-loader.asm)
;
.include "tn85def.inc"

; TX cell = 12: the real GBC only answered our $AA at cell 12 (5/5 valid $33 in
; a cell sweep; cells 11,13,14,15 got zero replies). RX uses
; the re-anchoring decoder (gap-per-pulse), RX_CELL 13 rounds those gaps fine.
.equ TCG_CELL  = 12
.equ TCG_PULSE = 4
.equ TCG_REST  = 8         ; 4 + 8 = 12
.equ TCG_GAP   = 40        ; idle before each byte's start pulse
.equ RX_CELL   = 13        ; re-anchoring decoder: round(gap/RX_CELL)
.equ RX_HALF   = 6
.equ RX_SPAN   = 120

.equ SND_WAIT = 250        ; 2 ms
.equ CHUNK    = 128

; mark/space link, in 8 us timer ticks (gbcpop MS_*_US; see header)
.equ MS_MARK   = 6         ; 48 us  LED on per bit
.equ MS_SPACE0 = 8         ; 64 us  space after the mark = bit 0
.equ MS_SPACE1 = 28        ; 224 us space after the mark = bit 1
.equ MS_LEAD   = 100       ; 800 us idle before each byte (GB frame sync)
.equ RX_THRESH = 42        ; 336 us: GB mark-to-mark '0' ~190 / '1' ~490 us
.equ TRIES     = 8         ; copies of one chunk before the round is given up
.equ ACK_WIN   = 16        ; ACK window in Timer0 overflows (x 2.05 ms = 33 ms)
.equ WRITE_TRIES = 3       ; attempts per Stage-0 chunk write (Card Pop! RPC)
.equ REQ_LISTEN = 12       ; idle loop: listen for a REQ, x ~33 ms (ACK_WIN)

.def LEN = r16
.def WAIT_TIME = r17

.cseg
.org 0x0000
	ldi r16, high(RAMEND)
	sts SPH, r16
	ldi r16, low(RAMEND)
	sts SPL, r16

	ldi r16, (1<<CLKPCE)
	out CLKPR, r16
	clr r16
	out CLKPR, r16

	ldi r16, (1<<DDB0)|(1<<DDB2)|(1<<DDB4)
	out DDRB, r16
	ldi r16, (1<<PORTB1)
	out PORTB, r16

	ldi r16, (1<<FOC0A)|(1<<CS01)|(1<<CS00)
	out TCCR0B, r16
	ldi r16, (1<<CS13)|(1<<CS11)|(1<<CS10)
	out TCCR1, r16

.ifdef DEBUG
	clr r16
	sts EE_HI, r16            ; dumps go to the lower EEPROM half by default
.endif

	ldi r16, 1                ; power-on: one blue blink
	ldi r19, (1<<PORTB2)
	rcall blink

main_loop:
	clr r16
	sts STARTED, r16
.ifdef DEBUG
	sts PROGRESS, r16
	sts SUBSTEP, r16
.endif
	rcall do_upload           ; the TCG's Card Pop! screen answers: Stage 0
	brne ml_no_round
.ifdef DEBUG
	ldi r16, 10               ; bootstrap running on the GBC
	sts SUBSTEP, r16
.endif
	rcall do_loader           ; Stage 1: loader + menu (r16 = REQ id if it
	brne round_fail           ; already came; a chunk never got through)
	tst r16
	breq main_loop            ; menu up: its REQ comes in a listen phase
	rcall send_payload
	rjmp main_loop

ml_no_round:
	lds r18, STARTED          ; the TCG answered, then the round broke off
	tst r18
	brne round_fail
	; No TCG: listen for a REQ (the loader's menu, e.g. after a payload
	; returned). The loader sends it after ~66 ms of dark only, i.e. never into
	; the ~33 ms gaps of our $AA probes, always into this listen phase.
	ldi r21, REQ_LISTEN
ml_listen:
	rcall ms_recv_byte_to     ; r16, Z set iff a byte came (preserves r21)
	brne ml_next
	tst r16
	breq ml_next
	cpi r16, NPAYLOAD + 1
	brsh ml_next              ; not a payload id: noise
	rcall send_payload
	rjmp main_loop
ml_next:
	dec r21
	brne ml_listen
	rjmp main_loop

round_fail:
	rcall leds_off
.ifdef DEBUG
	rcall dump_eeprom
.endif
	ldi r16, 3                ; 3 red blinks, then ~1 s dark
	ldi r19, (1<<PORTB4)
	rcall blink
	ldi r16, 5
rf_pause:
	rcall pause
	dec r16
	brne rf_pause
	rjmp main_loop

; ===================== the whole upload sequence ========================
; Returns Z set iff the bootstrap was written and the call was sent.
do_upload:
	; loop state
	ldi r16, low(boot_data << 1)
	sts UP_SRC, r16
	ldi r16, high(boot_data << 1)
	sts UP_SRC+1, r16
	ldi r16, low(BOOT_DEST)
	sts UP_DEST, r16
	ldi r16, high(BOOT_DEST)
	sts UP_DEST+1, r16
	ldi r16, low(BOOT_LEN)
	sts UP_REM, r16
	ldi r16, high(BOOT_LEN)
	sts UP_REM+1, r16
	ldi r16, WRITE_TRIES
	sts UP_TRY, r16

up_loop:
	lds r18, UP_REM
	lds r19, UP_REM+1
	mov r0, r18
	or r0, r19
	brne up_more            ; long branch to up_writes_done
	rjmp up_writes_done
up_more:

	; chunk = min(CHUNK, UP_REM) in r20
	tst r19
	brne up_full
	ldi r20, CHUNK
	cp r18, r20
	brsh up_have
	mov r20, r18
	rjmp up_have
up_full:
	ldi r20, CHUNK
up_have:
	; CMD: a=3, hl=0, de=UP_DEST, bc=chunk
	ldi r16, 3
	sts CMD_A, r16
	clr r16
	sts CMD_HL, r16
	sts CMD_HL+1, r16
	lds r16, UP_DEST
	sts CMD_DE, r16
	lds r16, UP_DEST+1
	sts CMD_DE+1, r16
	mov r16, r20
	sts CMD_BC, r16
	clr r16
	sts CMD_BC+1, r16
	; Z = flash source
	lds r30, UP_SRC
	lds r31, UP_SRC+1
	rcall remote_write        ; r20 = chunk (preserved across the call)
	breq up_wr_ok
	; A bad ack (hardware: reproducibly one bit off on the first attempt after
	; power-up) or no sync: write the same chunk again rather than restarting
	; the whole round. Rewriting the same bytes is harmless and repairs them.
	lds r16, STARTED          ; nothing ever answered: this was only the idle
	sbrs r16, 0               ; loop's probe, keep it short (one burst, ~0.4 s)
	rjmp up_fail
	lds r16, UP_TRY
	dec r16
	sts UP_TRY, r16
	brne up_have
	rjmp up_fail
up_wr_ok:
	ldi r16, WRITE_TRIES
	sts UP_TRY, r16

	; advance src/dest/rem by chunk (r20)
	clr r19                   ; 0 for carry adds
	lds r18, UP_SRC
	add r18, r20
	sts UP_SRC, r18
	lds r18, UP_SRC+1
	adc r18, r19
	sts UP_SRC+1, r18
	lds r18, UP_DEST
	add r18, r20
	sts UP_DEST, r18
	lds r18, UP_DEST+1
	adc r18, r19
	sts UP_DEST+1, r18
	lds r18, UP_REM
	sub r18, r20
	sts UP_REM, r18
	lds r18, UP_REM+1
	sbc r18, r19
	sts UP_REM+1, r18

.ifdef DEBUG
	lds r18, PROGRESS
	inc r18
	sts PROGRESS, r18
.endif
	rjmp up_loop

up_writes_done:
	; remote_call $C000 (the bootstrap resets VBK etc. itself)
	ldi r16, 4
	sts CMD_A, r16
	ldi r16, low(BOOT_DEST)
	sts CMD_HL, r16
	ldi r16, high(BOOT_DEST)
	sts CMD_HL+1, r16
	clr r16
	sts CMD_DE, r16
	sts CMD_DE+1, r16
	sts CMD_BC, r16
	sts CMD_BC+1, r16
	rcall send_command        ; no reply expected after cmd 4
	brne up_fail

	ser r18                   ; success
	rjmp up_ret
up_fail:
	clr r18
up_ret:
	cpi r18, 0xFF             ; Z set iff success
	ret

; ===================== RPC: write a block =============================
; In: Z = flash source, r20 = length, CMD_* already set (a=3, de, bc=len).
; Sends command+data as one contiguous stream, reads and verifies the ack.
; Returns Z set iff the ack matched. Preserves r20.
remote_write:
	push r18
	push r20
	push r24
	rcall send_command
	brne rw_fail
	clr r24                   ; data checksum
rw_data:
	tst r20
	breq rw_data_done
	lpm r16, Z+
	add r24, r16
	rcall tcg_send_byte
	dec r20
	rjmp rw_data
rw_data_done:
	mov r16, r24             ; data checksum = -sum
	neg r16
	rcall tcg_send_byte
	rcall tcg_recv_byte      ; r16 = ack, Z = start pulse found
.ifdef DEBUG
	push r18                 ; record the ack for diagnosis
	push r19
	ldi r18, 0xAC
	mov r19, r16
	rcall trace_rec
	pop r19
	pop r18
.endif
	brne rw_fail             ; no ack at all
	mov r18, r24
	neg r18                  ; expected ack = -sum
	cp r16, r18
	brne rw_fail
	ser r18
	rjmp rw_ret
rw_fail:
	clr r18
rw_ret:
	cpi r18, 0xFF
	pop r24
	pop r20
	pop r18
	ret

; ===================== RPC: send a command ============================
; In: CMD_A, CMD_HL(2), CMD_DE(2), CMD_BC(2). Syncs, sends "IR" + 8-byte packet
; + checksum. Returns Z set iff the sync succeeded. Does not touch Z(reg).
send_command:
	push r24
	push r25
	rcall sync_as_sender
	brne sc_fail
	ldi r16, 0x49
	rcall tcg_send_byte
	ldi r16, 0x52
	rcall tcg_send_byte
	clr r25                  ; packet checksum
	clr r16                  ; f = 0
	rcall pkt_send
	lds r16, CMD_A
	rcall pkt_send
	lds r16, CMD_HL
	rcall pkt_send
	lds r16, CMD_HL+1
	rcall pkt_send
	lds r16, CMD_DE
	rcall pkt_send
	lds r16, CMD_DE+1
	rcall pkt_send
	lds r16, CMD_BC
	rcall pkt_send
	lds r16, CMD_BC+1
	rcall pkt_send
	mov r16, r25
	neg r16                  ; packet checksum = -sum
	rcall tcg_send_byte
	ser r24
	rjmp sc_ret
sc_fail:
	clr r24
sc_ret:
	cpi r24, 0xFF            ; Z set iff sync ok
	pop r25
	pop r24
	ret

pkt_send:
	add r25, r16
	rcall tcg_send_byte
	ret

; send $AA until $33 comes back. Returns Z set iff $33 seen.
sync_as_sender:
	push r18
	push r19
	ldi r19, 12
sas_loop:
	ldi r16, 0xAA
	rcall tcg_send_byte
	rcall tcg_recv_byte
	brne sas_next
	cpi r16, 0x33
	breq sas_ok
sas_next:
	dec r19
	brne sas_loop
.ifdef DEBUG
	lds r18, STARTED       ; failed mid-round: record it (not the idle probes)
	tst r18
	breq sas_fail
	ldi r18, 0x5F
	clr r19
	rcall trace_rec
sas_fail:
.endif
	clr r18
	rjmp sas_ret
sas_ok:
.ifdef DEBUG
	lds r18, STARTED       ; a round's first sync: a new trace
	tst r18
	brne sas_started
	rcall trace_clr
sas_started:
.endif
	ldi r18, 1
	sts STARTED, r18       ; a handshake succeeded -> a transaction has started
	rcall led_ok           ; sending, and the GBC answers: blue
sas_ret:
	cpi r18, 1              ; Z set iff ok
	pop r19
	pop r18
	ret

; =============================== TCG TX =================================
tcg_cell_pulse:
	push WAIT_TIME
	sbi PORTB, PORTB0
	ldi WAIT_TIME, TCG_PULSE
	rcall wait
	cbi PORTB, PORTB0
	ldi WAIT_TIME, TCG_REST
	rcall wait
	pop WAIT_TIME
	ret

tcg_cell_idle:
	push WAIT_TIME
	cbi PORTB, PORTB0
	ldi WAIT_TIME, TCG_CELL
	rcall wait
	pop WAIT_TIME
	ret

tcg_send_byte:
	push r16
	push r19
	push WAIT_TIME
	cbi PORTB, PORTB0
	ldi WAIT_TIME, TCG_GAP
	rcall wait
	rcall tcg_cell_pulse
	ldi r19, 8
tsb_bit:
	sbrc r16, 0
	rjmp tsb_idle
	rcall tcg_cell_pulse
	rjmp tsb_next
tsb_idle:
	rcall tcg_cell_idle
tsb_next:
	lsr r16
	dec r19
	brne tsb_bit
	cbi PORTB, PORTB0
	pop WAIT_TIME
	pop r19
	pop r16
	ret

; =============================== TCG RX =================================
; Wait for a start pulse then decode 8 data cells. r16 = byte, Z set iff a
; start pulse was seen. Uses r22=dt, r23=prev, r24=found.
tcg_recv_byte:
	push r18
	push r19
	push r20
	push r22
	push r23
	push r24
	push r25
	rcall begin_receive
	brne trb_notfound
	clr r18
	out TCNT0, r18
	ldi r18, (1<<TOV0)
	out TIFR, r18
	ldi r16, 0xFF
	clr r23
	clr r24                ; cur_cell = 0 (the start pulse)
	clr r25                ; prev_t = 0
trb_loop:
	in r20, PINB
	in r22, TCNT0
	andi r20, (1<<PINB1)
	tst r20
	brne trb_setprev
	tst r23
	breq trb_setprev
	rcall advance_cell     ; re-anchor: gap to previous pulse -> step
	mov r25, r22
trb_setprev:
	mov r23, r20
	cpi r22, RX_SPAN
	brsh trb_done
	in r18, TIFR
	sbrs r18, TOV0
	rjmp trb_loop
trb_done:
	ldi r19, 1
	rjmp trb_ret
trb_notfound:
	clr r19
trb_ret:
	cpi r19, 1
	pop r25
	pop r24
	pop r23
	pop r22
	pop r20
	pop r19
	pop r18
	ret

; gap = r22 - r25; step = round(gap/RX_CELL); r24 += step; clear bit(r24-1) if 1..8
advance_cell:
	push r18
	push r19
	mov r18, r22
	sub r18, r25
	subi r18, -RX_HALF
	clr r19
ac_div:
	cpi r18, RX_CELL
	brlo ac_have
	subi r18, RX_CELL
	inc r19
	rjmp ac_div
ac_have:
	add r24, r19
	tst r24
	breq ac_ret
	cpi r24, 9
	brsh ac_ret
	mov r19, r24
	ldi r18, 1
	dec r19
ac_shift:
	tst r19
	breq ac_apply
	lsl r18
	dec r19
	rjmp ac_shift
ac_apply:
	com r18
	and r16, r18
ac_ret:
	pop r19
	pop r18
	ret

; ===================== Stage 1 + 2: loader, menu, payload ===============
; Stage 1, called right after cmd 4 started the bootstrap: the loader, the menu
; font and the menu text. Returns Z set on success, with r16 = the REQ id if the
; GB's REQ already came during the menu's ACK windows (else 0); Z clear if a
; chunk was never ACKed (the round is then retried from Stage 0).
do_loader:
	ldi r19, 10               ; 20 ms: the bootstrap first waits for vblank (up
dl_boot_wait:                 ; to ~17 ms) to colour the screen, deaf to IR;
	ldi WAIT_TIME, 250        ; sent sooner, the loader's first copy is missed
	rcall wait
	dec r19
	brne dl_boot_wait
	clr r23                   ; no REQ expected in these ACK windows
	ldi r30, low(loader_frame << 1)
	ldi r31, high(loader_frame << 1)
	ldi r24, low(LOADER_FRAME_LEN)
	ldi r25, high(LOADER_FRAME_LEN)
	rcall send_stream
	brtc dl_fail
	ldi r30, low(font_frame << 1)
	ldi r31, high(font_frame << 1)
	ldi r24, low(FONT_FRAME_LEN)
	ldi r25, high(FONT_FRAME_LEN)
	rcall send_stream
	brtc dl_fail
	ldi r23, NPAYLOAD         ; menu: the GB's REQ may come instead of the ACK
	ldi r30, low(menu_frame << 1)
	ldi r31, high(menu_frame << 1)
	ldi r24, low(MENU_FRAME_LEN)
	ldi r25, high(MENU_FRAME_LEN)
	rcall send_stream
	brtc dl_fail
.ifdef DEBUG
	ldi r18, 20               ; menu is up on the GBC
	sts SUBSTEP, r18
.endif
	rcall leds_off            ; menu shown: dark while the user chooses
	sez
	ret
dl_fail:
.ifdef DEBUG
	ldi r18, 0xCF             ; trace: a stream failed (r19 = its ID)
	rcall trace_rec
.endif
	clz
	ret

; Stage 2: send payload r16 (1..NPAYLOAD), its manifest and body; the loader
; applies it and jumps. On a failure the loader returns to its menu and resends
; the REQ once, which the idle loop picks up: nothing to retry here.
; Clobbers r16-r25, Z.
send_payload:
.ifdef DEBUG
	mov r19, r16
	ldi r18, 0xE0             ; trace: REQ id
	rcall trace_rec
.endif
	rcall led_ok              ; sending the payload
	; Z = payload_table + (id-1)*8 -> man addr, man len, body addr, body len
	dec r16
	lsl r16
	lsl r16
	lsl r16
	ldi r30, low(payload_table << 1)
	ldi r31, high(payload_table << 1)
	add r30, r16
	clr r16
	adc r31, r16
	lpm r20, Z+               ; manifest frame (word address)
	lpm r21, Z+
	lpm r24, Z+               ; manifest frame length
	lpm r25, Z+
	lpm r22, Z+               ; body frame (word address)
	lpm r23, Z+
	lpm r18, Z+               ; body frame length
	lpm r19, Z+
	push r22
	push r23
	push r18
	push r19
	clr r23                   ; no REQ in these ACK windows
	movw r30, r20
	lsl r30                   ; word -> byte address for lpm
	rol r31
	rcall send_stream         ; manifest
	pop r25
	pop r24
	pop r31
	pop r30
	brtc sp_fail
	lsl r30
	rol r31
	clr r23
	rcall send_stream         ; body -> the loader applies it and jumps (after
	brtc sp_fail               ; lingering ~100 ms to re-ACK a repeated last
.ifdef DEBUG                   ; chunk, so no ACK = not received)
	ldi r16, 40
	sts SUBSTEP, r16
	ldi r16, 1                ; the last payload sent: upper EEPROM half
	sts EE_HI, r16
	rcall dump_eeprom
	clr r16
	sts EE_HI, r16
.endif
	rjmp leds_off
sp_fail:
.ifdef DEBUG
	ldi r18, 0xCF             ; trace: a stream failed (r19 = its ID)
	rcall trace_rec
	rcall dump_eeprom
.endif
	rjmp leds_off

; Send a stream: its chunk frames lie back to back at Z (flash byte address),
; r25:r24 bytes in total. Each frame's length is in its header (len16 at +5,
; +9 for header/checksum). r23 = highest REQ id accepted in place of the last
; chunk's ACK (menu only), else 0. Returns T set and r16 = REQ id or 0 when
; every chunk was ACKed (or the REQ came); T clear and r19 = stream ID if a
; chunk never got through. Clobbers r16, r19, r24, r25, Z.
send_stream:
	push r22
	push r23
	push r26
	push r27
	movw r26, r24             ; X = stream bytes left
ss_frame:
	mov r16, r26
	or r16, r27
	breq ss_ok
	push r30
	push r31
	adiw r30, 3
	lpm r19, Z+               ; ID (reported on failure)
	lpm r22, Z+               ; SEQ: bit 7 = last chunk
	lpm r24, Z+               ; wire length
	lpm r25, Z
	pop r31
	pop r30
	adiw r24, 9               ; + header and checksum = frame length
	push r23
	sbrs r22, 7
	clr r23                   ; not the last chunk: no REQ expected yet
	rcall send_chunk
	pop r23
	brtc ss_ret               ; never ACKed (T clear)
	tst r16
	brne ss_ret               ; the REQ arrived (T set)
	add r30, r24              ; next frame
	adc r31, r25
	sub r26, r24
	sbc r27, r25
	rjmp ss_frame
ss_ok:
	clr r16
	set
ss_ret:
	pop r27
	pop r26
	pop r23
	pop r22
	ret

; Send one chunk frame at Z, r25:r24 bytes long, up to TRIES times. After each
; copy listen ~33 ms: the GB ACKs an accepted chunk with the frame's CK_lo (its
; second-to-last byte) and we stop. r23 = highest REQ id accepted in place of
; the ACK (the GB cannot re-ACK the menu's last chunk once it shows the menu,
; so its REQ may be what we hear), else 0.
; Returns T set and r16 = 0 (ACKed) or the REQ id; T clear if never ACKed.
; Trace: [0xC0 + copy#, reply] per copy (reply FF = nothing heard).
; Preserves r24, r25, Z. Clobbers r16.
send_chunk:
	push r18
	push r19
	push r20
	push r30                  ; r20 = expected ACK = frame[len - 2]
	push r31
	add r30, r24
	adc r31, r25
	sbiw r30, 2
	lpm r20, Z
	pop r31
	pop r30
	ldi r18, TRIES
scp_copy:
	push r30
	push r31
	push r24
	push r25
scp_byte:
	mov r16, r24
	or r16, r25
	breq scp_sent
	lpm r16, Z+
	rcall ms_send_byte
	sbiw r24, 1
	rjmp scp_byte
scp_sent:
	pop r25
	pop r24
	pop r31
	pop r30
	rcall leds_off            ; dark while the GB looks for a quiet line to ACK
	push WAIT_TIME            ; 1 ms: let our receiver settle after our own last
	ldi WAIT_TIME, 125        ; pulse (the GB waits ~4 ms before it ACKs anyway)
	rcall wait
	pop WAIT_TIME
	rcall ms_recv_byte_to     ; r16, Z set iff a byte came within ~33 ms
.ifdef DEBUG
	push r18                  ; trace [0xC0 + copy#, reply] (reply FF = none)
	push r19
	in r19, SREG              ; (in/push/ldi/mov keep the flags)
	push r19
	ldi r19, 0xFF
	brne scp_tr
	mov r19, r16
scp_tr:
	push r19                  ; reply
	ldi r19, TRIES + 1
	sub r19, r18              ; copy number (r18 counts down)
	ori r19, 0xC0
	mov r18, r19
	pop r19
	rcall trace_rec
	pop r19
	out SREG, r19
	pop r19
	pop r18
.endif
	brne scp_next
	cp r16, r20
	breq scp_acked
	tst r16
	breq scp_next
	cp r23, r16
	brsh scp_done             ; 1..r23: the REQ (r16 = id, T set below)
scp_next:
	rcall led_miss            ; this copy got no ACK: red until the next one does
	dec r18
	brne scp_copy
	clt                       ; never ACKed
	rjmp scp_ret
scp_acked:
	clr r16
scp_done:
	rcall led_ok              ; ACKed (or the REQ came): blue
	set
scp_ret:
	pop r20
	pop r19
	pop r18
	ret

; ============================ mark/space TX ============================
; r16 = byte, MSB first: lead idle, then per bit a mark and a space whose
; length is the bit, then a trailing mark that bounds bit 0's space (exactly
; gbcpop's ms_tx_byte). Clobbers r16.
ms_send_byte:
	push r19
	push WAIT_TIME
	cbi PORTB, PORTB0
	ldi WAIT_TIME, MS_LEAD
	rcall wait
	ldi r19, 8
msb_bit:
	sbi PORTB, PORTB0
	ldi WAIT_TIME, MS_MARK
	rcall wait
	cbi PORTB, PORTB0
	ldi WAIT_TIME, MS_SPACE0
	sbrc r16, 7
	ldi WAIT_TIME, MS_SPACE1
	rcall wait
	lsl r16
	dec r19
	brne msb_bit
	sbi PORTB, PORTB0         ; trailing mark
	ldi WAIT_TIME, MS_MARK
	rcall wait
	cbi PORTB, PORTB0
	ldi WAIT_TIME, MS_SPACE0
	rcall wait
	pop WAIT_TIME
	pop r19
	ret

; ============================ mark/space RX ============================
; A GB byte = 9 marks; the 8 mark-to-mark gaps are the bits, MSB first (long =
; 1). Light = PB1 low.

; An ACK window: listen for a byte for the whole ~33 ms and stay silent all
; that time. Timed with Timer0 overflows (8 us ticks, one overflow = 2.05 ms,
; ACK_WIN of them), not Timer1: Timer1 was never shown to give 33 ms here (the
; Card Pop! code only uses it for replies that come quickly), and on hardware
; the ATtiny sent its next copy too early for the GB to ever ACK (docs/08).
; A false start (e.g. our receiver still ringing from our own last pulse) does
; not end the window. r16 = byte; Z set iff one was decoded.
ms_recv_byte_to:
	push r18
	push r19
	push r20
	push r21
	ldi r21, ACK_WIN
	clr r18
	out TCNT0, r18
	ldi r18, (1<<TOV0)
	out TIFR, r18
mrt_idle:
	rcall mrt_tick
	breq mrt_none
	sbis PINB, PINB1          ; wait for light off (skip a mark in progress)
	rjmp mrt_idle
mrt_mark:
	rcall mrt_tick
	breq mrt_none
	sbic PINB, PINB1          ; wait for a mark
	rjmp mrt_mark
	rcall rx_bits             ; (uses Timer0 too; the window pauses meanwhile)
	breq mrt_ret              ; a byte (Z set)
	clr r18                   ; a false start: keep listening
	out TCNT0, r18
	ldi r18, (1<<TOV0)
	out TIFR, r18
	rjmp mrt_idle
mrt_none:
	clz
mrt_ret:
	pop r21
	pop r20
	pop r19
	pop r18
	ret

; Count Timer0 overflows for the ACK window: Z set when the window is over.
mrt_tick:
	in r18, TIFR
	sbrs r18, TOV0
	rjmp mrt_tick_run
	ldi r18, (1<<TOV0)
	out TIFR, r18
	dec r21                   ; Z set at 0
	ret
mrt_tick_run:
	clz
	ret

; At the first mark (light on): time the 8 gaps to the following marks with
; Timer0 (8 us ticks; a gap > ~2 ms = overflow = fail). r16 = byte; Z set iff
; all 8 arrived. Clobbers r18, r19, r20.
rx_bits:
	clr r16
	ldi r19, 8
rxb_bit:
	clr r18                   ; time this gap from the mark just seen
	out TCNT0, r18
	ldi r18, (1<<TOV0)
	out TIFR, r18
rxb_off:
	in r18, TIFR
	sbrc r18, TOV0
	rjmp rxb_fail
	sbis PINB, PINB1          ; wait for the mark to end
	rjmp rxb_off
rxb_on:
	in r18, TIFR
	sbrc r18, TOV0
	rjmp rxb_fail
	sbic PINB, PINB1          ; wait for the next mark
	rjmp rxb_on
	in r20, TCNT0
	lsl r16
	cpi r20, RX_THRESH
	brlo rxb_zero
	ori r16, 1
rxb_zero:
	dec r19
	brne rxb_bit
	sez
	ret
rxb_fail:
	clz
	ret

; =============================== helpers ================================
; LEDs: blue (PB2) = sending and the GBC answers, red (PB4) = the last copy got
; no ACK, both off = idle / waiting for the user / done. Both go dark during the
; ACK windows (invisible), so they can never disturb the GB's darkness check.
led_ok:
	cbi PORTB, PORTB4
	sbi PORTB, PORTB2
	ret
led_miss:
	cbi PORTB, PORTB2
	sbi PORTB, PORTB4
	ret
leds_off:
	cbi PORTB, PORTB2
	cbi PORTB, PORTB4
	ret

wait:
	push r18
	out OCR0A, WAIT_TIME
	clr r18
	out TCNT0, r18
	ldi r18, (1<<OCF0A)
	out TIFR, r18
wait_loop:
	in r18, TIFR
	sbrs r18, OCF0A
	rjmp wait_loop
	pop r18
	ret

; Blink an LED r16 times (~0.2 s on, ~0.2 s off); r19 = its PORTB mask
; (1<<PORTB2 blue, 1<<PORTB4 red). Preserves the other pins (IR LED, pull-up).
blink:
	push r16
	push r18
	tst r16
	breq bl_done
bl_next:
	in r18, PORTB
	or r18, r19
	out PORTB, r18
	rcall pause
	in r18, PORTB
	com r19
	and r18, r19
	com r19
	out PORTB, r18
	dec r16
	breq bl_done
	rcall pause
	rjmp bl_next
bl_done:
	pop r18
	pop r16
	ret

; ~0.2 s (100 x SND_WAIT).
pause:
	push r18
	push WAIT_TIME
	ldi WAIT_TIME, SND_WAIT
	ldi r18, 100
pa_loop:
	rcall wait
	dec r18
	brne pa_loop
	pop WAIT_TIME
	pop r18
	ret

.ifdef DEBUG
ee_write:
	push r20
ee_wait:
	sbic EECR, EEPE
	rjmp ee_wait
	clr r20
	out EECR, r20
	lds r20, EE_HI            ; 0 = lower half (last round), 1 = upper (last success)
	out EEARH, r20
	out EEARL, r19
	out EEDR, r18
	sbi EECR, EEMPE
	sbi EECR, EEPE
	pop r20
	ret

dump_eeprom:
	push r0
	in r0, SREG
	push r16
	push r18
	push r19
	push r26
	push r27
	clr r19
	lds r18, PROGRESS
	rcall ee_write
	inc r19
	lds r18, SUBSTEP
	rcall ee_write
	inc r19
	lds r18, TRACE_PTR
	rcall ee_write
	inc r19
	lds r18, TRACE_WRAPPED
	rcall ee_write
	inc r19
	ldi r26, low(TRACE)
	ldi r27, high(TRACE)
	ldi r16, 160
dump_next:
	ld r18, X+
	rcall ee_write
	inc r19
	dec r16
	brne dump_next
	pop r27
	pop r26
	pop r19
	pop r18
	pop r16
	out SREG, r0
	pop r0
	ret

trace_rec:
	push r0
	in r0, SREG
	push r20
	push r26
	push r27
	lds r20, TRACE_PTR
	ldi r26, low(TRACE)
	ldi r27, high(TRACE)
	add r26, r20
	clr r20
	adc r27, r20
	st X+, r18
	st X, r19
	lds r20, TRACE_PTR
	subi r20, -2
	cpi r20, 160
	brlo trace_nowrap
	clr r20
	push r18
	ldi r18, 1
	sts TRACE_WRAPPED, r18
	pop r18
trace_nowrap:
	sts TRACE_PTR, r20
	pop r27
	pop r26
	pop r20
	out SREG, r0
	pop r0
	ret

trace_clr:
	push r18
	clr r18
	sts TRACE_PTR, r18
	sts TRACE_WRAPPED, r18
	pop r18
	ret

.endif

begin_receive:
	push r18
	push r19
	push r20
	clr r19
	clr r20
	clr r18
	out TCNT1, r18
	ldi r18, (1<<TOV1)
	out TIFR, r18
wait_loop2:
	sbic PINB, PINB1
	ldi r19, 1
	sbis PINB, PINB1
	ldi r20, 1
	and r20, r19
	brne got_edge
	in r18, TIFR
	sbrs r18, TOV1
	rjmp wait_loop2
got_edge:
	cpi r20, 1
	pop r20
	pop r19
	pop r18
	ret

; ============================== payload ================================
.equ BOOT_DEST = 0xC000
.ifdef DEBUG
.include "loader_data_debug.inc"  ; generated by make DEBUG=1 (DEBUG bootstrap)
.else
.include "loader_data.inc"
.endif

; ================================ data =================================
.dseg
.org SRAM_START
.ifdef DEBUG
PROGRESS:       .byte 1
SUBSTEP:        .byte 1
TRACE:          .byte 160
TRACE_PTR:      .byte 1
TRACE_WRAPPED:  .byte 1
EE_HI:          .byte 1    ; EEPROM half for dump_eeprom: 0 lower, 1 upper
.endif
CMD_A:          .byte 1
CMD_HL:         .byte 2
CMD_DE:         .byte 2
CMD_BC:         .byte 2
UP_SRC:         .byte 2
UP_DEST:        .byte 2
UP_REM:         .byte 2
UP_TRY:         .byte 1    ; Stage-0 write attempts left for the current chunk
STARTED:        .byte 1    ; 1 once a sync ($33) succeeded this round
