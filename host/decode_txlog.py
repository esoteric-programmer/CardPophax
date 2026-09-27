#!/usr/bin/env python3
# Decode GB->host bytes from a VBA IR dump, polarity-aware.
# VBA logs the GB's LED inverted: the bit-carrying element is the VBA "ON->OFF"
# duration (the GB's IR_OFF space). short(~99cT)=0, long(~339cT)=1. A large
# ON->OFF (>2000) is an inter-byte idle gap / boundary.
import re, sys
log = sys.argv[1] if len(sys.argv) > 1 else '/tmp/cal_vba.log'
ev = []
for l in open(log):
    m = re.search(r'\[cT=(\d+)\] TX (ON|OFF)', l)
    if m: ev.append((int(m.group(1)), m.group(2)))
carr = []  # GB spaces (bit carriers) = VBA ON->OFF durations, in order
for i in range(len(ev) - 1):
    if ev[i][1] == 'ON' and ev[i+1][1] == 'OFF':
        carr.append(ev[i+1][0] - ev[i][0])
sym = ''.join('0' if d < 200 else ('1' if d < 2000 else '|') for d in carr)
groups = [g for g in sym.split('|') if g]
out = []
for g in groups:
    if len(g) >= 8:
        out.append(int(g[:8], 2))
print("GB TX bytes (%d):" % len(out), ' '.join('%02X' % v for v in out))
