"""ATtiny85 IR board — schematic source (Schemdraw).

Renders schematic.svg next to this file. See docs/04-attiny-hardware.md.

    python3 -m venv venv && venv/bin/pip install schemdraw
    venv/bin/python schematic.py
    inkscape schematic.svg --export-type=png --export-dpi=90 \
        --export-background=white --export-background-opacity=1 -o schematic.png

U1 = MCP607-I/P (dual op-amp, PDIP-8), drawn as two amplifiers U1A/U1B
plus a separate power unit U1C.  Pinout:
  1 OUTA  2 INA-  3 INA+  4 VSS  5 INB+  6 INB-  7 OUTB  8 VDD
U2 = ATtiny85 (DIP-8):
  PB0 (5) IR LED out, PB1 (6) IR receiver in (internal pull-up),
  PB2 (7) blue LED, PB4 (3) red LED, PB3 (2) free, PB5/RESET (1) reset button to GND
J2 = optional Raspberry Pi GPIO header (2x20), used with U2 removed:
  pin 2 +5 V, 6 GND, 11 GPIO17 (middle of R3a/R3b), 12 GPIO18 (= PB1 line),
  15 GPIO22 (= PB2 line: the blue status LED D1 through R1),
  13 GPIO27 (= RESET line: SW1 to GND, the Pi's pull-up; shutdown button)
"""
from pathlib import Path

import schemdraw
import schemdraw.elements as elm

schemdraw.use('svg')  # pure-SVG backend, no matplotlib needed

PIN_FS = 9     # font size for pin numbers
RAIL_Y = 0.6   # height of the receiver-output wire back to PB1


def pin(el, anchor, num, ofst=(0.25, 0.2)):
    """Small pin-number label next to an anchor point."""
    x, y = getattr(el, anchor)
    elm.Label().at((x + ofst[0], y + ofst[1])).label(num, fontsize=PIN_FS)


with schemdraw.Drawing(file=str(Path(__file__).with_name('schematic.svg'))) as d:
    d.config(unit=2.5, fontsize=12)

    # ------------------------------------------------------------------
    # U2: ATtiny85 with status LEDs and IR transmitter
    # (IcPin lists run bottom -> top on the left and right sides)
    # ------------------------------------------------------------------
    u2 = elm.Ic(
        pins=[
            elm.IcPin(name='PB0', pin='5', side='left'),
            elm.IcPin(name='PB4', pin='3', side='left'),
            elm.IcPin(name='PB2', pin='7', side='left'),
            elm.IcPin(name='PB5/RESET', pin='1', side='right', anchorname='RESET'),
            elm.IcPin(name='PB3', pin='2', side='right'),
            elm.IcPin(name='PB1', pin='6', side='right'),
            elm.IcPin(name='VCC', pin='8', side='top'),
            elm.IcPin(name='GND', pin='4', side='bottom'),
        ],
        pinspacing=1.5, edgepadW=1.2, edgepadH=1.5,
    ).right().anchor('PB0').at((14, -6)).label('U2\nATtiny85', loc='top', ofst=(1.4, 0.1))
    elm.Vdd().at(u2.VCC).label('+5 V')
    elm.Ground().at(u2.GND)

    # LED branches, staircased so no vertical crosses another pin's wire
    # IR LED resistor split 200 + 200: the Pi (J2, GPIO17) feeds the middle,
    # so in Raspberry Pi mode only R3b is in series with the LED
    elm.Resistor().left().at(u2.PB0).label('R3a  200 Ω')
    n_r3 = elm.Dot().center
    elm.Resistor().left().label('R3b  200 Ω')
    elm.LED().down().label('D3\nSFH 4544\n(IR)', loc='top')
    elm.Ground()

    elm.Resistor().left().at(u2.PB4).label('R2  330 Ω')
    elm.Line().left(5.5)
    elm.LED().down().label('D2\nred', loc='top')
    elm.Ground()

    elm.Line().left(0.75).at(u2.PB2)
    n_pb2 = elm.Dot().center                  # J2 GPIO22 joins here (Pi mode)
    elm.Resistor().left().label('R1  220 Ω')
    elm.Line().left(9.75)
    elm.LED().down().label('D1\nblue', loc='top')
    elm.Ground()
    elm.Line().up(1.25).at(n_pb2)
    elm.Tag(width=1.9).left().label('GPIO22')

    elm.Line().down(2.25).at(n_r3)
    elm.Tag(width=1.9).right().label('GPIO17')

    elm.NoConnect().at(u2.PB3)
    elm.Line().right(1.25).at(u2.RESET)
    n_rst = elm.Dot().center                  # J2 GPIO27 joins here (Pi mode)
    elm.Button().down().label('SW1\nRESET', loc='bottom')   # push -> RESET low
    elm.Ground()
    elm.Line().right(0.75).at(n_rst)
    elm.Tag(width=1.9).right().label('GPIO27')

    # ------------------------------------------------------------------
    # U1A: transimpedance amplifier for the photodiode
    # ------------------------------------------------------------------
    op1 = elm.Opamp(leads=True).right().flip().anchor('in1').at((30, -2.5))
    op1.label('U1A\nMCP607', loc='top', ofst=(0.8, 0.1))
    pin(op1, 'in2', '3')
    pin(op1, 'in1', '2')
    pin(op1, 'out', '1', ofst=(-0.35, 0.2))

    elm.Line().left(0.75).at(op1.in2)
    elm.Ground().down()                           # IN+ at GND

    elm.Line().left(1).at(op1.in1)
    n_inv = elm.Dot().center
    elm.Line().left(2)
    elm.Photodiode().down().reverse().label('D4\nSFH 205 F', loc='top')
    elm.Ground()                                  # anode GND, cathode -> IN-

    elm.Line().right(0.75).at(op1.out)
    n_out1 = elm.Dot().center

    # feedback: 50k || 120p
    elm.Line().down(2).at(n_inv)
    elm.Dot()
    fb_r = elm.Resistor().right().tox(n_out1).label('R4  49.9 kΩ')
    elm.Dot()
    elm.Line().toy(n_out1)
    elm.Line().down(1.5).at(fb_r.start)
    elm.Capacitor().right().tox(n_out1).label('C1  120 pF', loc='bottom')
    elm.Line().toy(fb_r.end)

    # ------------------------------------------------------------------
    # AC coupling, high-pass, diode clamp
    # ------------------------------------------------------------------
    elm.Capacitor().right().at(n_out1).label('C2  47 nF')
    n_hp = elm.Dot().center
    elm.Resistor().down().label('R5\n49.9 kΩ', loc='top')
    elm.Ground()

    elm.Line().right(2).at(n_hp)
    n_plus2 = elm.Dot().center
    elm.Resistor().down().label('R6\n100 Ω', loc='bottom')
    elm.Schottky().down().reverse().label('D5\nBAT85', loc='bottom')  # cathode up, clamps negative spikes
    elm.Ground()

    # ------------------------------------------------------------------
    # U1B: non-inverting amplifier, gain 1 + 20k/2k = 11
    # ------------------------------------------------------------------
    elm.Line().right(3).at(n_plus2)
    op2 = elm.Opamp(leads=True).right().flip().anchor('in2')
    op2.label('U1B\nMCP607', loc='top', ofst=(0.8, 0.1))
    pin(op2, 'in2', '5')
    pin(op2, 'in1', '6')
    pin(op2, 'out', '7', ofst=(-0.35, 0.2))

    elm.Line().right(0.75).at(op2.out)
    n_out2 = elm.Dot().center

    elm.Line().left(0.5).at(op2.in1)
    elm.Line().down(2)
    n_fb2 = elm.Dot().center
    elm.Resistor().right().tox(n_out2).label('R7  20 kΩ', loc='bottom')
    elm.Line().toy(n_out2)
    elm.Resistor().down().at(n_fb2).label('R8\n2 kΩ', loc='bottom')
    elm.Ground()

    # ------------------------------------------------------------------
    # Open-collector output back to PB1 (light received -> PB1 low)
    # ------------------------------------------------------------------
    elm.Resistor().right().at(n_out2).label('R9  20 kΩ')
    q1 = elm.BjtNpn(circle=True).right().anchor('base').label('Q1\nBC337-40', loc='right')
    elm.Ground().at(q1.emitter)
    elm.Line().up().at(q1.collector).toy(RAIL_Y)
    elm.Line().left().tox(u2.PB1[0] + 3.5)
    elm.Line().down().toy(u2.PB1)
    n_pb1 = elm.Dot().center
    elm.Line().left().tox(u2.PB1)
    elm.Line().right(0.75).at(n_pb1)
    elm.Tag(width=1.9).right().label('GPIO18')

    # ------------------------------------------------------------------
    # Power: J1 input, +5 V rail, decoupling caps, U1C (op-amp supply pins)
    # ------------------------------------------------------------------
    j1 = elm.Header(rows=2, pinsleft=['+5 V', 'GND'], pinspacing=1).anchor('pin1').at((0, -11))
    j1.label('J1  5 V DC in', loc='top')
    elm.Line().right(0.75).at(j1.pin2)
    elm.Ground()
    elm.Line().right(2).at(j1.pin1)
    elm.Dot()
    elm.Vdd().label('+5 V')
    elm.Line().right(2).at((2, -11))
    elm.Dot()
    elm.Capacitor().down().label('C3\n100 nF\n(at U2)', loc='bottom')
    elm.Ground()
    elm.Line().right(4).at((4, -11))
    elm.Dot()
    elm.Capacitor().down().label('C4\n100 nF\n(at U1)', loc='bottom')
    elm.Ground()
    elm.Line().right(4).at((8, -11))
    pwr = elm.Ic(
        pins=[
            elm.IcPin(name='VDD', pin='8', side='top'),
            elm.IcPin(name='VSS', pin='4', side='bottom'),
        ],
        size=(2, 2.5), pinspacing=1,
    ).right().anchor('VDD').label('U1C\nMCP607', loc='right', ofst=0.3)
    elm.Ground().at(pwr.VSS)

    # ------------------------------------------------------------------
    # J2: Raspberry Pi GPIO header (alternative to U2)
    # ------------------------------------------------------------------
    j2 = elm.Ic(
        pins=[
            elm.IcPin(name='GND', pin='6', side='right'),
            elm.IcPin(name='GPIO27', pin='13', side='right'),
            elm.IcPin(name='GPIO22', pin='15', side='right'),
            elm.IcPin(name='GPIO18', pin='12', side='right'),
            elm.IcPin(name='GPIO17', pin='11', side='right'),
            elm.IcPin(name='5V', pin='2', side='right'),
        ],
        pinspacing=1, edgepadW=1.2, edgepadH=0.4,
    ).right().anchor('5V').at((22, -11)).label('J2\nRaspberry Pi\n2×20 header', loc='left', ofst=0.3)
    elm.Line().right(0.75).at(j2['5V'])
    elm.Vdd().label('+5 V')
    elm.Line().right(0.75).at(j2.GPIO17)
    elm.Tag(width=1.9).right().label('GPIO17')
    elm.Line().right(0.75).at(j2.GPIO18)
    elm.Tag(width=1.9).right().label('GPIO18')
    elm.Line().right(0.75).at(j2.GPIO22)
    elm.Tag(width=1.9).right().label('GPIO22')
    elm.Line().right(0.75).at(j2.GPIO27)
    elm.Tag(width=1.9).right().label('GPIO27')
    elm.Line().right(0.75).at(j2.GND)
    elm.Ground()
    elm.Label().at((27, -11)).label(
        'J2 is for Raspberry Pi mode only:\n'
        'remove U2 (ATtiny85) from its socket first,\n'
        'and do not power the board from J1 at the same time.',
        halign='left', valign='top')

    # ------------------------------------------------------------------
    # Assembly note (placement is not visible from the wiring)
    # ------------------------------------------------------------------
    elm.Label().at((27, -14)).label(
        'Assembly note:\n'
        'Mount D3 (IR LED) and D4 (photodiode) next to each other\n'
        'at an edge or corner of the board, both facing outward\n'
        'in the same direction.',
        halign='left', valign='top')
