#!/usr/bin/env python3
"""
extract_mon.py — take one Pokémon out of a Western save and write it as an
include file for save-patcher.asm: the party struct, OT name and nickname,
byte for byte, plus the species, Pokédex number and ASCII name.

  extract_mon.py gen1 <save.sav> [--slot 1] [--dex N] [--prefix MEW] [-o mew.inc]
      a party Pokémon of Red / Blue / Yellow (44-byte party struct, as stored)
  extract_mon.py gen2 <save.sav> --box B --slot S [--base hp,atk,def,spd,sat,sdf]
                                  [--prefix CELEBI] [-o celebi.inc]
      a boxed Pokémon of Gold / Silver / Crystal (32-byte box struct); the
      party-only tail (status, HP, stats) is computed with the Gen-2 formula
  extract_mon.py gen1to2 <save.sav> [--slot 1] [--prefix MEW2] [-o mew2.inc]
      a Red / Blue / Yellow party Pokémon converted the way the Time Capsule
      does it (pokecrystal engine/link/link.asm, Function2868a/.ConvertToGen2):
      held item from the catch rate (TimeCapsule_CatchRateItems: Mew's 45 ->
      Bitter Berry), SpAtk/SpDef computed, friendship 70, Pokérus and caught
      data 0; everything else copied

Layouts: pokered / pokegold / pokecrystal sram.asm, PKHeX SAV1/SAV2 offsets.
The save's checksum must be valid. --dex / --base are only needed for species
other than Mew (Gen 1) and Celebi (Gen 2).
"""

import argparse
import math
import sys

B1 = 0x2000
NAME_LEN = 11

# Gen 1 (pokered): party at $AF2C, checksum over $A598..$B522 at $B523
G1_PARTY, G1_CK_FROM, G1_CK = B1 + 0x0F2C, B1 + 0x0598, B1 + 0x1523
# Gen 2: main checksum over $A009..end-1, stored little-endian
G2 = {
    "gs": dict(ck_end=B1 + 0x0D69, cur_box=B1 + 0x0724, active_box=B1 + 0x0D6C),
    "c":  dict(ck_end=B1 + 0x0B83, ck=B1 + 0x0D0D, cur_box=B1 + 0x0700, active_box=B1 + 0x0D10),
}
G2["gs"]["ck"] = G2["gs"]["ck_end"]
BOX_SIZE, BOX_MONS = 0x450, 20

GEN1_DEX = {0x15: 151}                     # internal index -> Pokédex (Mew)
GEN2_BASE = {251: (100,) * 6, 151: (100,) * 6}   # Celebi, Mew
# pokecrystal data/items/catch_rate_items.asm (Time Capsule): catch rate ->
# held item where it differs; any other catch rate is taken as the item id
CATCH_RATE_ITEMS = {0x19: 0x92, 0x2D: 0x53, 0x32: 0xAE, 0x5A: 0xAD, 0x64: 0xAD,
                    0x78: 0xAD, 0x87: 0xAD, 0xBE: 0xAD, 0xC3: 0xAD, 0xDC: 0xAD,
                    0xFA: 0xAD, 0xFF: 0xAD}


def to_ascii(name):
    out = ""
    for b in name:
        if b == 0x50:
            break
        if 0x80 <= b <= 0x99:
            out += chr(ord("A") + b - 0x80)
        elif 0xF6 <= b <= 0xFF:
            out += chr(ord("0") + b - 0xF6)
        else:
            out += "?"
    return out


def gen1(d, a):
    if (~sum(d[G1_CK_FROM:G1_CK])) & 0xFF != d[G1_CK]:
        sys.exit("bad Gen-1 checksum: not a Western Red/Blue/Yellow save?")
    n = d[G1_PARTY]
    if not 1 <= a.slot <= n <= 6:
        sys.exit(f"party has {n} Pokémon, slot {a.slot} does not exist")
    i = a.slot - 1
    mon = d[G1_PARTY + 8 + 44 * i:][:44]
    ot = d[G1_PARTY + 8 + 264 + NAME_LEN * i:][:NAME_LEN]
    nick = d[G1_PARTY + 8 + 330 + NAME_LEN * i:][:NAME_LEN]
    dex = a.dex or GEN1_DEX.get(mon[0])
    if not dex:
        sys.exit(f"species ${mon[0]:02X}: pass --dex")
    where = f"party slot {a.slot}"
    info = f"ID {mon[12] << 8 | mon[13]}, level {mon[33]}"
    return mon, ot, nick, dex, where, info


def gen2_stat(base, dv, statexp, level, hp):
    bonus = min(255, math.isqrt(max(0, statexp - 1)) + 1) // 4 if statexp else 0
    v = ((base + dv) * 2 + bonus) * level // 100
    return v + level + 10 if hp else v + 5


def gen2(d, a):
    game = next((g for g, L in G2.items()
                 if d[B1 + 8] == 99 and sum(d[B1 + 9:L["ck_end"]]) & 0xFFFF
                 == d[L["ck"]] | d[L["ck"] + 1] << 8), None)
    if not game:
        sys.exit("bad Gen-2 checksum: not a Western Gold/Silver/Crystal save?")
    L = G2[game]
    if not (1 <= a.box <= 14 and 1 <= a.slot <= BOX_MONS):
        sys.exit("--box 1..14, --slot 1..20")
    if (d[L["cur_box"]] & 0x7F) == a.box - 1:
        base = L["active_box"]             # the current box lives in bank 1
    else:
        k = a.box - 1
        base = (0x4000 if k < 7 else 0x6000) + (k % 7) * BOX_SIZE
    n = d[base]
    if not a.slot <= n <= BOX_MONS:
        sys.exit(f"box {a.box} has {n} Pokémon, slot {a.slot} does not exist")
    i = a.slot - 1
    box = d[base + 22 + 32 * i:][:32]
    ot = d[base + 22 + 32 * BOX_MONS + NAME_LEN * i:][:NAME_LEN]
    nick = d[base + 22 + 43 * BOX_MONS + NAME_LEN * i:][:NAME_LEN]
    species, level = box[0], box[31]
    stats = a.base or GEN2_BASE.get(species)
    if not stats:
        sys.exit(f"species {species}: pass --base hp,atk,def,spd,sat,sdf")
    atk, dfn, spd, spc = box[21] >> 4, box[21] & 15, box[22] >> 4, box[22] & 15
    dvs = [((atk & 1) << 3) | ((dfn & 1) << 2) | ((spd & 1) << 1) | (spc & 1),
           atk, dfn, spd, spc, spc]
    sexp = [box[11 + 2 * k] << 8 | box[12 + 2 * k] for k in range(5)]
    sexp.append(sexp[4])                   # special attack and defense share it
    st = [gen2_stat(stats[k], dvs[k], sexp[k], level, k == 0) for k in range(6)]
    tail = bytes([0, 0]) + b"".join(v.to_bytes(2, "big") for v in [st[0]] + st)
    where = f"box {a.box} slot {a.slot} ({game.upper()} layout)"
    info = f"ID {box[6] << 8 | box[7]}, level {level}, stats {st}"
    return bytes(box) + tail, ot, nick, species, where, info


def gen1to2(d, a):
    g1, ot, nick, dex, where, info = gen1(d, a)
    stats = a.base or GEN2_BASE.get(dex)
    if not stats:
        sys.exit(f"dex {dex}: pass --base hp,atk,def,spd,sat,sdf")
    level, spc_dv, spc_exp = g1[33], g1[28] & 15, g1[25] << 8 | g1[26]
    s = bytearray(48)
    s[0] = dex                             # Gen-2 species = Pokédex number
    s[1] = CATCH_RATE_ITEMS.get(g1[7], g1[7]) if g1[7] else 0
    s[2:27] = g1[8:33]                     # moves, ID, exp, stat exp, DVs, PP
    s[27] = 70                             # friendship; Pokérus, caught data 0
    s[31] = level
    s[32] = g1[4]                          # status
    s[34:36] = g1[1:3]                     # current HP
    s[36:44] = g1[34:42]                   # max HP, attack, defense, speed
    for k, base in ((44, stats[4]), (46, stats[5])):
        s[k:k + 2] = gen2_stat(base, spc_dv, spc_exp, level, False).to_bytes(2, "big")
    where += ", Time Capsule conversion"
    info += f", item ${s[1]:02X}, SpAtk/SpDef {s[45]}/{s[47]}"
    return bytes(s), ot, nick, dex, where, info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("gen", choices=["gen1", "gen2", "gen1to2"])
    ap.add_argument("sav")
    ap.add_argument("--slot", type=int, default=1)
    ap.add_argument("--box", type=int)
    ap.add_argument("--dex", type=int)
    ap.add_argument("--base", type=lambda s: tuple(int(x) for x in s.split(",")))
    ap.add_argument("--prefix")
    ap.add_argument("-o", "--out")
    a = ap.parse_args()

    d = open(a.sav, "rb").read()[:0x8000]  # drop an emulator's RTC footer
    if len(d) < 0x8000:
        sys.exit("not a 32 KB save")
    mon, ot, nick, dex, where, info = {"gen1": gen1, "gen2": gen2, "gen1to2": gen1to2}[a.gen](d, a)
    name = to_ascii(nick)
    p = a.prefix or name
    out = a.out or p.lower() + ".inc"

    def db(b):
        return "    db " + ", ".join(f"${x:02X}" for x in b)
    half = len(mon) // 2
    with open(out, "w") as f:
        f.write(f"; GENERATED by extract_mon.py from {a.sav.split('/')[-1]}, {where}\n"
                f"; {name}, OT {to_ascii(ot)}, {info}\n"
                f"DEF {p}_SPECIES EQU ${mon[0]:02X}\n"
                f"DEF {p}_DEX EQU {dex}\n"
                f'DEF {p}_NAME EQUS "{name}"\n'
                f"MACRO {p}_DATA\n"
                f"{db(mon[:half])}\n{db(mon[half:])} ; party struct\n"
                f"{db(ot)} ; OT name\n{db(nick)} ; nickname\n"
                "ENDM\n")
    print(f"{out}: {name} (species ${mon[0]:02X}, dex {dex}), OT {to_ascii(ot)}, {info}")


main()
