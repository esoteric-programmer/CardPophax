#!/usr/bin/env python3
"""
gen2save.py — inspect and edit a Pokémon Gold / Silver / Crystal save image
dumped with `gbcpop swap-dump`, and inject a Celebi.

Layout and checksum taken from the pokecrystal disassembly.  All Western
localisations (including the German "Pokémon Kristall") share this structure;
the script proves it before touching anything by recomputing the stored
checksum, and refuses to write if that does not match.

  sCheckValue1        $A008   = 99
  sGameData           $A009 .. $AB82      (checksummed region)
  sPlayerID           $A009   dw, big-endian
  sPlayerName         $A00B   11 bytes, $50-terminated
  sPartyCount         $A865
  sPartySpecies       $A866   6 bytes, then $FF at $A86C
  sPartyMon[i]        $A86D + 48*i
  sPartyMonOT[i]      $A98D + 11*i
  sPartyMonNick[i]    $A9CF + 11*i
  sPokedexCaught      $AA27   32 bytes, flag array
  sPokedexSeen        $AA47   32 bytes, flag array
  sChecksum           $AD0D   dw, little-endian, plain 16-bit sum of $A009..$AB82

usage:
  gen2save.py info    <save.bin>
  gen2save.py celebi  <in.bin> <out.bin> [--level 30] [--slot next] [--shiny]
                                         [--nick CELEBI] [--ot-from-save]
"""

import argparse
import struct
import sys

BANK_SIZE       = 0x2000
SRAM_BASE       = 0xA000

CHECK_VALUE_1   = 99
CHECK_VALUE_2   = 127

A_CHECKVALUE1   = 0xA008
A_GAMEDATA      = 0xA009
A_GAMEDATA_END  = 0xAB83          # exclusive
A_PLAYER_ID     = 0xA009
A_PLAYER_NAME   = 0xA00B
A_PARTY_COUNT   = 0xA865
A_PARTY_SPECIES = 0xA866
A_PARTY_MONS    = 0xA86D
A_PARTY_OT      = 0xA98D
A_PARTY_NICK    = 0xA9CF
A_DEX_CAUGHT    = 0xAA27
A_DEX_SEEN      = 0xAA47
A_CHECKSUM      = 0xAD0D
A_CHECKVALUE2   = 0xAD0F

PARTY_STRUCT    = 48
NAME_LENGTH     = 11
PARTY_LENGTH    = 6

CELEBI          = 251
CELEBI_BASE     = dict(hp=100, atk=100, dfn=100, spd=100, sat=100, sdf=100)

# move id, pp  (pokecrystal: constants/move_constants.asm, data/moves/moves.asm)
MOVES = {
    "LEECH_SEED":   (73, 10),
    "CONFUSION":    (93, 25),
    "RECOVER":     (105, 20),
    "HEAL_BELL":   (215,  5),
    "SAFEGUARD":   (219, 25),
    "ANCIENTPOWER":(246,  5),
    "FUTURE_SIGHT":(248, 15),
    "BATON_PASS":  (226, 40),
    "PERISH_SONG": (195,  5),
}
# level-up order from data/pokemon/evos_attacks.asm
CELEBI_LEARNSET = [
    (1,  "LEECH_SEED"), (1, "CONFUSION"), (1, "RECOVER"), (1, "HEAL_BELL"),
    (10, "SAFEGUARD"), (20, "ANCIENTPOWER"), (30, "FUTURE_SIGHT"),
    (40, "BATON_PASS"), (50, "PERISH_SONG"),
]

# pokecrystal charmap.asm, Western character set
CHARMAP = {" ": 0x7F, "@": 0x50, "(": 0x9A, ")": 0x9B, ":": 0x9C, ";": 0x9D,
           "[": 0x9E, "]": 0x9F, "-": 0xE3, "?": 0xE6, "!": 0xE7, ".": 0xE8,
           "é": 0xEA, "Ä": 0xC0, "Ö": 0xC1, "Ü": 0xC2, "ä": 0xC3, "ö": 0xC4,
           "ü": 0xC5}
for _i, _c in enumerate("ABCDEFGHIJKLMNOPQRSTUVWXYZ"): CHARMAP[_c] = 0x80 + _i
for _i, _c in enumerate("abcdefghijklmnopqrstuvwxyz"): CHARMAP[_c] = 0xA0 + _i
for _i in range(10):                                    CHARMAP[str(_i)] = 0xF6 + _i
REVMAP = {v: k for k, v in CHARMAP.items()}


# --------------------------------------------------------------------------

class Save:
    """A raw multi-bank SRAM image, with one bank selected as the save bank."""

    def __init__(self, blob):
        if len(blob) % BANK_SIZE:
            raise ValueError("image is not a whole number of 8 KiB banks")
        self.banks = [bytearray(blob[i:i + BANK_SIZE])
                      for i in range(0, len(blob), BANK_SIZE)]
        self.bank = self._find_save_bank()

    def _find_save_bank(self):
        """
        The layout is an assumption; these four tests are the proof. A blank
        bank passes the checksum trivially (0 == 0), so the two sentinel bytes
        the game writes carry the real weight.
        """
        for i, b in enumerate(self.banks):
            if b[self._off(A_CHECKVALUE1)] != CHECK_VALUE_1:
                continue
            if b[self._off(A_CHECKVALUE2)] != CHECK_VALUE_2:
                continue
            if b[self._off(A_PARTY_COUNT)] > PARTY_LENGTH:
                continue
            if self._checksum(b) != self._stored(b):
                continue
            return i
        raise ValueError(
            "no bank looks like a valid Gen 2 save (sentinels $%02X/$%02X plus a "
            "matching checksum). Either this is not a Gold/Silver/Crystal dump, "
            "or the dump is corrupt — re-dump before writing anything."
            % (CHECK_VALUE_1, CHECK_VALUE_2))

    @staticmethod
    def _off(addr):
        return addr - SRAM_BASE

    @classmethod
    def _checksum(cls, bank):
        lo, hi = cls._off(A_GAMEDATA), cls._off(A_GAMEDATA_END)
        return sum(bank[lo:hi]) & 0xFFFF

    @classmethod
    def _stored(cls, bank):
        o = cls._off(A_CHECKSUM)
        return bank[o] | (bank[o + 1] << 8)

    # -- accessors ---------------------------------------------------------
    def rd(self, addr, n=1):
        o = self._off(addr)
        return bytes(self.banks[self.bank][o:o + n])

    def wr(self, addr, data):
        o = self._off(addr)
        self.banks[self.bank][o:o + len(data)] = data

    def reseal(self):
        b = self.banks[self.bank]
        b[self._off(A_CHECKVALUE1)] = CHECK_VALUE_1
        b[self._off(A_CHECKVALUE2)] = CHECK_VALUE_2
        c = self._checksum(b)
        b[self._off(A_CHECKSUM)] = c & 0xFF
        b[self._off(A_CHECKSUM) + 1] = c >> 8

    def blob(self):
        return b"".join(bytes(b) for b in self.banks)

    # -- convenience -------------------------------------------------------
    @property
    def player_name(self):
        return decode(self.rd(A_PLAYER_NAME, NAME_LENGTH))

    @property
    def player_id(self):
        return struct.unpack(">H", self.rd(A_PLAYER_ID, 2))[0]

    @property
    def party_count(self):
        return self.rd(A_PARTY_COUNT)[0]


def decode(raw):
    out = []
    for b in raw:
        if b == 0x50:
            break
        out.append(REVMAP.get(b, "?"))
    return "".join(out)


def encode(text, length):
    out = bytearray()
    for ch in text[:length - 1]:
        if ch not in CHARMAP:
            raise ValueError("character %r has no Gen 2 code point" % ch)
        out.append(CHARMAP[ch])
    out.append(0x50)
    out.extend([0x50] * (length - len(out)))
    return bytes(out[:length])


# --------------------------------------------------------------------------

def exp_for_level(level, growth="MEDIUM_SLOW"):
    n = level
    if growth == "MEDIUM_SLOW":
        return max(0, (6 * n ** 3) // 5 - 15 * n ** 2 + 100 * n - 140)
    raise ValueError(growth)


def stat(base, dv, level, is_hp):
    """Gen 2 stat formula with zero stat experience."""
    v = ((base + dv) * 2 * level) // 100
    return v + level + 10 if is_hp else v + 5


def celebi_struct(level, dvs, ot_id, moves):
    """48 bytes, party_struct from pokecrystal/macros/wram.asm. Big-endian."""
    atk_dv, def_dv, spd_dv, spc_dv = dvs
    hp_dv = ((atk_dv & 1) << 3) | ((def_dv & 1) << 2) | \
            ((spd_dv & 1) << 1) | (spc_dv & 1)

    hp  = stat(CELEBI_BASE["hp"],  hp_dv,  level, True)
    atk = stat(CELEBI_BASE["atk"], atk_dv, level, False)
    dfn = stat(CELEBI_BASE["dfn"], def_dv, level, False)
    spd = stat(CELEBI_BASE["spd"], spd_dv, level, False)
    sat = stat(CELEBI_BASE["sat"], spc_dv, level, False)
    sdf = stat(CELEBI_BASE["sdf"], spc_dv, level, False)

    exp = exp_for_level(level)
    m = bytearray(4)
    pp = bytearray(4)
    for i, name in enumerate(moves):
        m[i], pp[i] = MOVES[name]

    s = bytearray(PARTY_STRUCT)
    s[0] = CELEBI
    s[1] = 0                                   # held item: none
    s[2:6] = m
    s[6:8] = struct.pack(">H", ot_id)
    s[8:11] = exp.to_bytes(3, "big")
    s[11:21] = bytes(10)                       # stat experience
    s[21:23] = bytes([(atk_dv << 4) | def_dv, (spd_dv << 4) | spc_dv])
    s[23:27] = pp
    s[27] = 255                                # happiness
    s[28] = 0                                  # pokérus
    s[29] = (1 << 6) | (level & 0x3F)          # caught time morning / caught level
    s[30] = 0                                  # caught location: none
    s[31] = level
    s[32] = 0                                  # status
    s[33] = 0
    s[34:36] = struct.pack(">H", hp)
    s[36:38] = struct.pack(">H", hp)
    s[38:40] = struct.pack(">H", atk)
    s[40:42] = struct.pack(">H", dfn)
    s[42:44] = struct.pack(">H", spd)
    s[44:46] = struct.pack(">H", sat)
    s[46:48] = struct.pack(">H", sdf)
    return bytes(s), dict(hp=hp, atk=atk, dfn=dfn, spd=spd, sat=sat, sdf=sdf)


def moves_for_level(level):
    learned = [n for lv, n in CELEBI_LEARNSET if lv <= level and n in MOVES]
    return learned[-4:] if len(learned) > 4 else learned + [None] * (4 - len(learned))


def set_dex_flag(save, addr, species):
    idx = species - 1
    off = Save._off(addr) + idx // 8
    save.banks[save.bank][off] |= 1 << (idx % 8)


# --------------------------------------------------------------------------

def cmd_info(args):
    save = Save(open(args.image, "rb").read())
    print("save bank        : %d  (checksum verified)" % save.bank)
    print("trainer          : %s   ID %05d" % (save.player_name, save.player_id))
    n = save.party_count
    print("party            : %d Pokémon" % n)
    species = save.rd(A_PARTY_SPECIES, PARTY_LENGTH)
    for i in range(min(n, PARTY_LENGTH)):
        mon = save.rd(A_PARTY_MONS + i * PARTY_STRUCT, PARTY_STRUCT)
        nick = decode(save.rd(A_PARTY_NICK + i * NAME_LENGTH, NAME_LENGTH))
        ot = decode(save.rd(A_PARTY_OT + i * NAME_LENGTH, NAME_LENGTH))
        print("  %d. #%3d  Lv.%-3d  %-11s  OT %-8s  HP %d/%d"
              % (i + 1, species[i], mon[31], nick, ot,
                 struct.unpack(">H", mon[34:36])[0],
                 struct.unpack(">H", mon[36:38])[0]))
    return 0


def cmd_celebi(args):
    save = Save(open(args.infile, "rb").read())
    print("save bank        : %d  (checksum verified)" % save.bank)
    print("trainer          : %s   ID %05d" % (save.player_name, save.player_id))

    count = save.party_count
    if args.slot == "next":
        if count >= PARTY_LENGTH:
            sys.exit("party is full — pass --slot 1..6 to overwrite one")
        slot = count
        new_count = count + 1
    else:
        slot = int(args.slot) - 1
        if not 0 <= slot < PARTY_LENGTH or slot >= count:
            sys.exit("--slot must be 1..%d (an occupied slot)" % count)
        new_count = count

    dvs = (10, 10, 10, 10) if args.shiny else (15, 15, 15, 15)
    moves = [m for m in moves_for_level(args.level) if m]
    mon, stats = celebi_struct(args.level, dvs, save.player_id, moves)

    ot = save.rd(A_PLAYER_NAME, NAME_LENGTH) if args.ot_from_save \
         else encode(args.ot, NAME_LENGTH)
    nick = encode(args.nick, NAME_LENGTH)

    save.wr(A_PARTY_COUNT, bytes([new_count]))
    sp = bytearray(save.rd(A_PARTY_SPECIES, PARTY_LENGTH + 1))
    sp[slot] = CELEBI
    sp[new_count] = 0xFF
    save.wr(A_PARTY_SPECIES, bytes(sp))
    save.wr(A_PARTY_MONS + slot * PARTY_STRUCT, mon)
    save.wr(A_PARTY_OT + slot * NAME_LENGTH, ot)
    save.wr(A_PARTY_NICK + slot * NAME_LENGTH, nick)
    set_dex_flag(save, A_DEX_SEEN, CELEBI)
    set_dex_flag(save, A_DEX_CAUGHT, CELEBI)
    save.reseal()

    open(args.outfile, "wb").write(save.blob())
    print("slot             : %d%s" % (slot + 1, "" if new_count > count else " (overwritten)"))
    print("Celebi           : Lv.%d  %s  DVs %s" %
          (args.level, "shiny" if args.shiny else "normal", dvs))
    print("moves            : %s" % ", ".join(moves))
    print("stats            : HP %(hp)d  Atk %(atk)d  Def %(dfn)d  Spd %(spd)d  "
          "SpA %(sat)d  SpD %(sdf)d" % stats)
    print("OT               : %s" % decode(ot))
    print("checksum         : resealed")
    print("wrote %s" % args.outfile)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd")   # no required= : Python 3.2 compatibility

    p = sub.add_parser("info", help="show the trainer and party")
    p.add_argument("image")
    p.set_defaults(func=cmd_info)

    p = sub.add_parser("celebi", help="inject a Celebi and reseal the checksum")
    p.add_argument("infile")
    p.add_argument("outfile")
    p.add_argument("--level", type=int, default=30,
                   help="default 30, as in the Japanese GS Ball event")
    p.add_argument("--slot", default="next", help="'next' or 1..6")
    p.add_argument("--shiny", action="store_true", help="DVs 10/10/10/10")
    p.add_argument("--nick", default="CELEBI")
    p.add_argument("--ot", default="RASPI")
    p.add_argument("--ot-from-save", action="store_true",
                   help="use the save's own trainer name as the OT")
    p.set_defaults(func=cmd_celebi)

    args = ap.parse_args()
    if not getattr(args, "cmd", None):
        ap.print_help()
        return 2
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
