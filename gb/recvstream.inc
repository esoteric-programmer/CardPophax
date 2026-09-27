; recvstream.inc — chunked, checked, ACKed stream receive
; (docs/05-loader-protocol.md §3-§4; the why: docs/08-lessons-learned.md).
;
; A stream (ID) is sent as chunks of <= 128 raw bytes, each as its own frame:
;   $55 $55 START ID SEQ len_lo len_hi <len bytes> CK_lo CK_hi
; START $3C = body stored as-is, $3D = body RLE-compressed (per chunk; decoded
; on the fly by rle_put). SEQ bits 0-6 = chunk index, bit 7 = last chunk.
; len/CK describe the wire bytes; CK = 16-bit sum of ID, SEQ and body.
; The host repeats a chunk until we ACK it (stop-and-wait per chunk; a failed
; chunk costs one chunk, not the whole stream).
;
; Guards against false STARTs (a receiver often joins mid-chunk, and bodies
; contain $3C/$3D): START only counts right after a $55; ID and SEQ must match;
; a chunk is < 256 wire bytes and must fit the buffer before anything is stored.
;
; ACK: a chunk that passes its checksum is answered with its CK_lo once the
; line has been dark for ACK_IDLE polls (~4 ms, longer than the host's ~0.9 ms
; inter-byte gap, i.e. the host stopped and listens); if that does not happen
; within ACK_BUDGET polls (~17 ms) no ACK is sent. (Under test on hardware: the
; GB kept seeing light there until a hand shaded the sensor, docs/08;
; a fixed ~3 ms delay instead of this check is the alternative.)
;
; Repeats: if the host missed our ACK it sends the same chunk again. A chunk
; equal to the last accepted one (hLastId + hLastSeq) is checked but NOT stored
; -- its destination may be in use (the loader runs where a repeated loader
; chunk would land) -- and ACKed again. Anything else unexpected is ignored.
; ID and SEQ arrive before the data, so this is decided before any write.
;
; Screen status (DEF SCREEN_STATUS, bootstrap only): teal (the boot colour)
; after a chunk that passed its checksum -- set before its ACK, so a colour
; change's ~17 ms vblank wait falls into the host's listen window -- and red
; after a failed copy or when nothing arrived for QUIET_RED receive timeouts
; (~63 ms: copies missed completely, e.g. the IR path blocked).
;
; Timed receive (DEF LINGER, loader only): recv_checked_stream_timed gives up
; (carry) after n receive timeouts of silence without an ACKed chunk; the loader
; uses it after REQ, so a lost menu choice returns to the menu instead of
; waiting forever. recv_linger keeps re-ACKing repeats of the last accepted
; chunk until the line has been quiet for LINGER_TIMEOUTS receive timeouts
; (~100 ms), then returns. Called before the GB stops receiving (menu
; draw, payload jump), so a lost final ACK is re-sent, and a host that never
; gets one knows the chunk really did not arrive.
DEF START_MARK     EQU $3C
DEF START_MARK_RLE EQU $3D
DEF PREAMBLE       EQU $55
DEF ACK_IDLE       EQU 200     ; ~4 ms of continuous dark (normal speed)
DEF ACK_BUDGET     EQU 800     ; ~17 ms: give up, no ACK
DEF CHUNK_RAW_MAX  EQU 128     ; decoded bytes per chunk (host guarantees)
DEF LINGER_TIMEOUTS EQU 5      ; x ~21 ms (RX_SYNC_BUDGET) of quiet line
DEF QUIET_RED      EQU 3       ; x ~21 ms without a byte -> screen red (bootstrap)

; recv_checked_stream: hl = destination, b = buffer size in 256-byte pages,
; c = stream ID. Receives chunks 0, 1, ... up to the one flagged last, each
; ACKed, stored back to back from hl. Clobbers a, bc, de, hl.
recv_checked_stream::
	ld a, h
	add b
	ldh [hRxEnd], a            ; first page past the buffer
	ld a, c
	ldh [hRxId], a
	xor a
	ldh [hRxSeq], a
.attempt
	ld e, 0                    ; previous byte (for the preamble check)
.skip
	call ir_recv_byte          ; preserves bc, de, hl
IF DEF(LINGER)
	jp c, .timeout
ELIF DEF(SCREEN_STATUS)
	jr c, .quiet
ELSE
	jr c, .attempt
ENDC
	ld d, a
IF DEF(SCREEN_STATUS)
	xor a
	ldh [hQuiet], a            ; something arrives: not silent
ENDC
	ld a, e
	ld e, d                    ; previous := this
	cp PREAMBLE
	jr nz, .skip               ; START only counts right after a $55
	ld a, d
IF DEF(NO_RLE)
	cp START_MARK
	jr nz, .skip
ELSE
	sub START_MARK             ; $3C -> 0 (raw), $3D -> 1 (RLE)
	cp 2
	jr nc, .skip
	ldh [hRleOn], a
	xor a
	ldh [hRleLit], a
	ldh [hRleRep], a
ENDC
	push hl                    ; chunk start, restored on retry
	call ir_recv_byte          ; ID
	jr c, .bad
	ld c, a
	call ir_recv_byte          ; SEQ
	jr c, .bad
	ld d, a
	ldh [hRxCurSeq], a
	ldh a, [hRxId]
	cp c
	jr nz, .other
	ld a, d
	and $7F
	ld e, a
	ldh a, [hRxSeq]
	cp e
	ld a, 0
	jr z, .mode                ; our next chunk: store it
.other
	ldh a, [hLastId]
	cp c
	jr nz, .retry
	ldh a, [hLastSeq]
	cp d
	jr nz, .retry              ; unrelated: ignore
	ld a, 1                    ; the last accepted chunk again: check, ACK, don't store
	jr .mode
IF DEF(SCREEN_STATUS)
.quiet                         ; ~21 ms without a byte (RX_SYNC_BUDGET)
	ldh a, [hQuiet]
	inc a
	ldh [hQuiet], a
	cp QUIET_RED               ; longer than any pause the host makes (its
	jp nz, .attempt            ; ACK window + lead, ~37 ms): copies are
	ld a, SCREEN_BAD           ; missed completely (e.g. the path is blocked)
	call status_color
	jp .attempt
ENDC
.bad                           ; a copy failed: dropped byte / bad checksum
IF DEF(SCREEN_STATUS)
	ld a, SCREEN_BAD
	call status_color          ; (preserves bc, hl)
ENDC
.retry                         ; (mid-routine so every jr reaches it)
	pop hl                     ; back to the chunk start, try the next copy
	jp .attempt
.mode
	ldh [hRxSkip], a
	ld a, c                    ; bc = CK so far = ID + SEQ
	add d
	ld c, a
	ld a, 0
	adc a
	ld b, a
	call ir_recv_byte          ; len_lo
	jr c, .bad
	ld e, a
	call ir_recv_byte          ; len_hi
	jr c, .bad
	or a
	jr nz, .retry              ; a chunk is < 256 wire bytes
	ld d, a
	ldh a, [hRxSkip]
	or a
	jr nz, .rx                 ; nothing is stored: no bound needed
	ldh a, [hRxEnd]            ; the decoded chunk (<= CHUNK_RAW_MAX) must fit:
	ld d, a                    ; high(hl + CHUNK_RAW_MAX - 1) < end page
	ld a, l
	add CHUNK_RAW_MAX - 1
	ld a, h
	adc 0
	cp d
	ld d, 0
	jr nc, .retry
.rx
	ld a, d
	or e
	jr z, .rxdone
	call ir_recv_byte
	jr c, .bad
	push af
	add c                      ; bc += a
	ld c, a
	jr nc, .nocarry
	inc b
.nocarry
	ldh a, [hRxSkip]
	or a
	jr nz, .nostore
	pop af
IF DEF(NO_RLE)
	ld [hl+], a
ELSE
	call rle_put               ; store/decode a at hl (advances hl)
ENDC
	jr .next
.nostore
	pop af
.next
	dec de
	jr .rx
.rxdone
	call ir_recv_byte          ; CK_lo
	jr c, .bad
	cp c
	jr nz, .bad
	call ir_recv_byte          ; CK_hi
	jr c, .bad
	cp b
	jr nz, .bad
	pop de                     ; drop the saved chunk start; hl = next write
IF DEF(SCREEN_STATUS)
	ld a, SCREEN_OK            ; colour first, then the ACK: the host is already
	call status_color          ; listening, and the ACK then comes right before
ENDC                           ; the next chunk (none of it is missed)
	call send_ack
IF DEF(LINGER)
	ldh a, [hRxIdle]           ; a timed receive: every ACKed chunk (stored or
	or a                       ; a repeat) restarts the count
	jr z, .untimed
	ldh a, [hRxIdleMax]
	ldh [hRxIdle], a
.untimed
ENDC
	ldh a, [hRxSkip]
	or a
	jp nz, .attempt            ; a repeat: keep waiting (hl unchanged)
	ldh a, [hRxId]
	ldh [hLastId], a
	ldh a, [hRxCurSeq]
	ldh [hLastSeq], a
	bit 7, a
	ret nz                     ; the last chunk: stream complete
	ldh a, [hRxSeq]
	inc a
	ldh [hRxSeq], a
	jp .attempt
IF DEF(LINGER)
.timeout                       ; ~21 ms without a byte (RX_SYNC_BUDGET)
	ldh a, [hRxIdle]           ; 0: wait forever
	or a
	jp z, .attempt
	dec a
	ldh [hRxIdle], a
	jp nz, .attempt
	scf                        ; quiet too long: give up (carry set)
	ret
ENDC

IF DEF(SCREEN_STATUS)
; Bootstrap only: the whole screen shows the last chunk's outcome (teal = a
; chunk passed its checksum, red = a copy failed), like the ATtiny's LEDs.
; a = colour byte (set_screen_color, bootscreen.inc); only a change costs the
; wait for vblank (up to ~17 ms). Preserves bc, hl.
status_color:
	push bc
	ld b, a
	ldh a, [hScreen]
	cp b
	jr z, .same
	ld a, b
	ldh [hScreen], a
	call set_screen_color
.same
	pop bc
	ret
ENDC

IF DEF(LINGER)
; recv_checked_stream_timed: recv_checked_stream that gives up when a = n
; receive timeouts (~21 ms each) pass without an ACKed chunk (the count
; restarts with every one). Returns carry clear with the stream stored, or carry
; set on giving up. Same registers otherwise.
recv_checked_stream_timed:
	ldh [hRxIdleMax], a
	ldh [hRxIdle], a
	call recv_checked_stream
	push af
	xor a
	ldh [hRxIdle], a           ; back to untimed
	pop af
	ret

; recv_linger: re-ACK repeats of the last accepted chunk (nothing is stored:
; ID 0 never matches) until the line has been quiet ~100 ms. Clobbers a, bc,
; de, hl.
recv_linger:
	ld a, LINGER_TIMEOUTS
	ldh [hRxIdleMax], a
	ldh [hRxIdle], a
	ld hl, $C000               ; unused: nothing is stored
	ld bc, $0100               ; b = 1 page, c = ID 0 (never sent)
	call recv_checked_stream
	xor a
	ldh [hRxIdle], a
	ret
ENDC

; ACK the chunk just checked: once the line is dark, send its CK_lo (c).
; Returns carry set if no ACK was sent. Preserves hl. Clobbers a, bc, de.
send_ack:
	push bc
	call ack_wait_idle
	pop bc
	ret c                      ; the line never went dark: no ACK
	ld a, c
	call ir_send_byte
	or a                       ; carry clear: ACK sent
	ret

; Wait for ACK_IDLE consecutive dark polls; carry set if ACK_BUDGET polls pass
; first. Clobbers a, bc, e.
ack_wait_idle:
	ld bc, ACK_BUDGET
	ld e, 0
.poll
	ldh a, [rRP]
	bit 1, a
	jr z, .busy                ; light
	inc e
	ld a, e
	cp ACK_IDLE
	ret nc                     ; dark long enough (carry clear)
	jr .tick
.busy
	ld e, 0
.tick
	dec bc
	ld a, b
	or c
	jr nz, .poll
	scf
	ret

IF !DEF(NO_RLE)
; rle_put: feed one wire byte a to the decoder, writing output at hl (advanced).
; Preserves bc, de. Format (PackBits-like), a control byte then data:
;   $00-$7F  n+1 literal bytes follow
;   $80-$FF  the next byte is repeated (n & $7F)+1 times
; A run is written in one go between two IR bytes, so the host caps the run
; length (RLE_MAX_RUN in gbcpop.c) to fit the inter-byte idle gap.
rle_put:
	push bc
	ld c, a
	ldh a, [hRleOn]
	or a
	jr z, .store               ; raw stream
	ldh a, [hRleLit]
	or a
	jr nz, .lit
	ldh a, [hRleRep]
	or a
	jr nz, .rep
	ld a, c                    ; control byte
	bit 7, a
	jr nz, .setrep
	inc a                      ; n+1 literals
	ldh [hRleLit], a
	pop bc
	ret
.setrep
	and $7F
	inc a                      ; (n & $7F)+1 repeats
	ldh [hRleRep], a
	pop bc
	ret
.lit
	dec a
	ldh [hRleLit], a
.store
	ld [hl], c
	inc hl
	pop bc
	ret
.rep
	ld b, a
	xor a
	ldh [hRleRep], a
	ld a, c
.rl	ld [hl+], a
	dec b
	jr nz, .rl
	pop bc
	ret
ENDC
