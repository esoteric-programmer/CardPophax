; GBC IR port and loader constants (see docs/05-loader-protocol.md)
DEF rIE   EQU $FFFF
DEF rIF   EQU $FF0F
DEF rNR52 EQU $FF26          ; sound on/off (0 = APU off)
DEF rSVBK EQU $FF70          ; CGB WRAM bank select
DEF rKEY1 EQU $FF4D          ; CGB speed switch
DEF rP1   EQU $FF00          ; joypad
DEF rLCDC EQU $FF40
DEF MBC_RAMG EQU $0000        ; cart SRAM enable: $0A = on, $00 = off (MBC1/3/5)
DEF rSCY  EQU $FF42
DEF rSCX  EQU $FF43
DEF rLY   EQU $FF44
DEF rVBK  EQU $FF4F          ; CGB VRAM bank select (1 = BG attributes)
DEF rBCPS EQU $FF68          ; CGB BG palette index (bit7 = auto-increment)
DEF rBCPD EQU $FF69          ; CGB BG palette data
DEF rRP   EQU $FF56          ; IR port: bit0 write LED (1=on), bit1 read (0=light)
DEF IR_ON   EQU $C1          ; receiver enabled + LED on   (a mark)
DEF IR_OFF  EQU $C0          ; receiver enabled + LED off  (a space / idle)

; --- mark/space bit-layer timing (loop counts at normal speed; docs/05 §2) ---
; A "bit" = a short mark then a space; the SPACE length encodes the value.
; TX_* are ir_delay iterations (~3.8 us each). RX_* / FRAME_IDLE are polls of
; the receive loops (~15.3 us each); RX_THRESH splits short vs long.
DEF TX_MARK   EQU 14         ; LED on per bit
DEF TX_SPACE0 EQU 24         ; ~90us
DEF TX_SPACE1 EQU 105        ; ~390us (mark-to-mark ~450 vs 150)
DEF RX_THRESH EQU 9          ; ~137 us: host space ~64 us (~4 counts) vs ~224 us (~15)
DEF RX_TIMEOUT EQU 200       ; loop budget waiting for an edge
DEF FRAME_IDLE EQU 26        ; ~400 us idle = inter-byte gap (>~15 intra-space, <lead)
DEF RX_SYNC_BUDGET EQU 1400  ; ir_recv_byte sync poll budget (~20ms) -> quick NAK on drop

DEF BOOT_COLOR EQU $4A4A     ; CGB BGR555 screen colour while loading (teal); lo byte == hi byte
	ASSERT LOW(BOOT_COLOR) == HIGH(BOOT_COLOR)
; bootstrap screen status (recvstream.inc, SCREEN_STATUS); colours with lo == hi
DEF SCREEN_OK  EQU LOW(BOOT_COLOR) ; teal: the last chunk passed its checksum
DEF SCREEN_BAD EQU $1C         ; red ($1C1C): the last copy failed

; loader memory map (docs/05-loader-protocol.md §7)
DEF BOOT_ORG   EQU $C000     ; Stage-0 bootstrap upload target (<=512 B safe scratch)
DEF LOADER_BANK EQU 7        ; full loader lives in WRAM bank 7
DEF LOADER_ORG EQU $D000     ; full loader load address (bank 7)
DEF LOADER_SP  EQU $DFFF     ; loader stack top (bank 7)
DEF LOADER_MAX EQU $0800     ; max loader image (feed len guard; menu bufs at $D800)
DEF LOADER_RETURN EQU LOADER_ORG + 3 ; payloads: SVBK=7, jp here -> back to the menu

; stream IDs (frame header byte after START; recvstream.inc)
DEF ID_LOADER   EQU 'L'
DEF ID_FONT     EQU 'F'
DEF ID_MENU     EQU 'M'
DEF ID_MANIFEST EQU 'N'
DEF ID_BODY     EQU 'B'

; recvstream.inc state in HRAM, $FFEC-$FFF5: above the loader's HRAM trampoline
; ($FF80..$FFDF) and vars ($FFE0..$FFEB). Both bootstrap and loader keep their
; stack in WRAM bank 7 (LOADER_SP), so the rest of HRAM is free. hLastId/Seq
; must survive the bootstrap -> loader jump (that is how the loader recognises
; a repeat of the loader's last chunk).
DEF hRleOn    EQU $FFEC        ; 0 = raw chunk, else RLE
DEF hRleLit   EQU $FFED        ; literal bytes still to copy
DEF hRleRep   EQU $FFEE        ; nonzero: next byte is a run value, repeated this often
DEF hRxEnd    EQU $FFEF        ; first page past the destination buffer
DEF hRxId     EQU $FFF0        ; stream ID the current call waits for
DEF hLastId   EQU $FFF1        ; ID of the last chunk accepted
DEF hRxSkip   EQU $FFF2        ; nonzero: this chunk repeats the last one -> check, don't store
DEF hRxSeq    EQU $FFF3        ; chunk index expected next
DEF hLastSeq  EQU $FFF4        ; SEQ byte (incl. last flag) of the last chunk accepted
DEF hRxCurSeq EQU $FFF5        ; SEQ byte of the chunk being received
DEF hRxIdle   EQU $FFF6        ; linger mode: receive timeouts (~21 ms each) left, 0 = off
DEF hScreen   EQU $FFF7        ; bootstrap: colour byte on screen now (status_color)
DEF hQuiet    EQU $FFF8        ; bootstrap: receive timeouts in a row (screen red at QUIET_RED)
DEF hRxIdleMax EQU $FFF9       ; loader: hRxIdle reload value (timed receive / linger)
