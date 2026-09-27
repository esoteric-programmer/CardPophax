#!/usr/bin/env python3
"""
test_card_pop.py — headless test of the Card Pop! gift on fake TCG carts.

`make tests` builds the payload with the TCG header (test-TCG.gbc) and with a
Japanese destination (test-TCGJ.gbc, must be refused). The payload patches its
own cartridge, so no swap is needed. Each case seeds SRAM (by default from a
real save, see SAVE below), presses buttons on the menu, and compares every
SRAM byte with a model of what the game's own Card Pop! writes (pret/poketcg
engine/link/card_pop.asm, engine/save.asm, home/card_collection.asm). The
partner name's last two bytes are chosen at run time; the test checks they
make a new entry and satisfy the game's rare-branch (Mew) rule, for both cards.

Needs PyBoy (`pip install pyboy`). Run: make tests && ./test_card_pop.py [save]
"""

import io
import os
import sys
from pyboy import PyBoy

HERE = os.path.dirname(os.path.abspath(__file__))
SAVE = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser(
    "~/Schreibtisch/Pokemon Trading Card Game (Europe) (En,Fr,De) (SGB Enhanced).sav")

B2 = 0x4000                                   # .sav offset of SRAM bank 2
POPS, PLAYER, COLL, DECKS, ALBUM, NAMES = 0x0005, 0x0010, 0x0100, 0x0200, 0x18FE, 0x1B00
GENERAL = 0x1800
VENUSAUR, MEW, NUM_CARDS = 0x0A, 0xA1, 228
ATTINY = bytes([3, 0x30, 3, 0x43, 3, 0x43, 3, 0x38, 3, 0x3D, 3, 0x48, 0, 0])


def card_total(s, base, card):
    n = s[base + COLL + card] & 0x7F
    for k in range(4):
        d = base + DECKS + 0x54 * k
        if s[d]:
            n += sum(1 for c in s[d + 24:d + 84] if c == card)
    return n


def add_card(s, base, card):                  # AddCardToCollection + UpdateAlbumProgress
    if card_total(s, base, card) >= 99:
        return
    s[base + COLL + card] = (s[base + COLL + card] & 0x7F) + 1
    owned = sum(1 for i in range(256) if not s[base + COLL + i] & 0x80) & 0xFF
    total = NUM_CARDS - (s[base + COLL + VENUSAUR] >> 7) - (s[base + COLL + MEW] >> 7)
    s[base + ALBUM], s[base + ALBUM + 1] = owned, total


def card_pop(before, card, name):             # what a successful Card Pop! writes
    s = bytearray(before)
    old = s[POPS]
    s[POPS] = (old + 1) & 0xFF
    slot = NAMES + (old & 15) * 16
    s[slot:slot + 16] = name
    add_card(s, B2, card)
    add_card(s, 0, card)
    return s


def mew_rule(player, other):                  # DecideCardToReceiveFromCardPop
    e = (sum(player) - sum(other)) & 0xFF
    d = 0
    xp = xo = 0
    for b in player:
        xp ^= b
    for b in other:
        xo ^= b
    d = (xp - xo) & 0xFF
    return e == 5 and d & 1 == 1              # -> MEW_LV15


def names(s):
    return [bytes(s[NAMES + 16 * i:NAMES + 16 * i + 16]) for i in range(16)]


def screen_text(pb):
    rows = ("".join(chr(pb.memory[0x9800 + r * 32 + c]) for c in range(20)).rstrip()
            for r in range(18))
    return "\n".join(x for x in rows if x)


def first(t):
    return t.splitlines()[0] if t else ""


def run(code, ram, keys):
    pb = PyBoy(os.path.join(HERE, f"test-{code}.gbc"), window="null", cgb=True,
               sound_emulated=False, ram_file=io.BytesIO(bytes(ram)))
    pb.set_emulation_speed(0)
    pb.tick(240)
    prompt = screen_text(pb)
    results = []
    for key in keys:
        pb.button(key, 5)
        pb.tick(30)
        results.append((prompt, screen_text(pb)))
        pb.tick(200)
    pb.memory[0x0000] = 0x0A
    sram = bytearray()
    for bank in range(4):
        pb.memory[0x4000] = bank
        sram += bytes(pb.memory[0xA000:0xC000])
    pb.memory[0x0000] = 0
    back = screen_text(pb)
    pb.stop(save=False)
    return prompt, results, sram, back


def check_pops(before, after, cards):
    """Replay the model pop by pop, taking each name from the result."""
    s = bytearray(before)
    for card in cards:
        slot = NAMES + (s[POPS] & 15) * 16
        name = bytes(after[slot:slot + 16])
        if name[:14] != ATTINY or name in names(s):
            return False, "name not ATTINY or already listed"
        if not mew_rule(bytes(s[PLAYER:PLAYER + 16]), name):
            return False, "rare-branch (Mew) rule not met"
        s = card_pop(s, card, name)
    return s == after, "sram differs" if s != after else ""


base = bytearray(open(SAVE, "rb").read()[:0x8000])
fail = 0


def case(label, code, ram, keys, want, cards):
    global fail
    prompt, results, sram, back = run(code, ram, keys)
    shown = [first(r[1]) for r in results]
    if cards is None:
        ok_sram, why = (ram == sram), ("sram changed" if ram != sram else "")
    else:
        ok_sram, why = check_pops(ram, sram, cards)
    ok = (len(shown) == len(want) and all(x.startswith(w) for x, w in zip(shown, want))
          and ok_sram and "A: ADD MEW" in prompt and back == prompt)
    fail += not ok
    print(f"{'ok  ' if ok else 'FAIL'} {label:34} {shown} {why}")


def with_(s, **kw):
    s = bytearray(s)
    for off, v in kw.get("set", []):
        s[off] = v
    return s


def full_list(s, count):
    s = bytearray(s)
    s[POPS] = count
    for i in range(16):                        # 16 distinct partners
        s[NAMES + 16 * i:NAMES + 16 * i + 16] = bytes([3, 0x30 + i] + [0] * 12 + [i, 0x5A])
    return s


bad_save = with_(base, set=[(GENERAL + 8, base[GENERAL + 8] ^ 1)])
mew98 = with_(base, set=[(COLL + MEW, 98)])
# one Mew in deck 1 + 98 in the collection = 99 -> limit
deck_mew = bytearray(mew98)
deck_mew[DECKS + 24] = MEW
tcgj = bytearray(base)

case("A: Mew on the real save", "TCG", base, ["a"], ["DONE."], [MEW])
case("B: Venusaur on the real save", "TCG", base, ["b"], ["DONE."], [VENUSAUR])
case("A, A, B: three pops in a row", "TCG", base, ["a", "a", "b"], ["DONE."] * 3, [MEW, MEW, VENUSAUR])
case("SELECT (standalone): ignored", "TCG", base, ["select"], ["CARD POP GIFT"], None)
case("98 Mew: one more, then limit", "TCG", mew98, ["a", "a"], ["DONE.", "99 CARDS ALREADY"], [MEW])
case("98 + 1 in a deck: limit", "TCG", deck_mew, ["a"], ["99 CARDS ALREADY"], None)
case("general save checksum bad", "TCG", bad_save, ["a"], ["NO SAVE FOUND"], None)
case("Japanese TCG refused", "TCGJ", tcgj, ["a"], ["NOT SUPPORTED"], None)
case("full list, count 15 -> slot 15", "TCG", full_list(base, 15), ["a"], ["DONE."], [MEW])
case("full list, count 16 -> slot 0 (oldest)", "TCG", full_list(base, 16), ["b"], ["DONE."], [VENUSAUR])
case("count 255 -> 0 (8-bit wrap), slot 15", "TCG", full_list(base, 255), ["a", "a"], ["DONE."] * 2, [MEW, MEW])

# the generated name is listed already: the payload must choose another pair.
# PyBoy is deterministic, so a first run tells which name comes out.
for card, key in ((MEW, "a"), (VENUSAUR, "b")):
    _, _, first_run, _ = run("TCG", base, [key])
    slot = NAMES + (base[POPS] & 15) * 16
    taken = bytes(first_run[slot:slot + 16])
    seeded = bytearray(base)
    seeded[NAMES + 16 * 9:NAMES + 16 * 10] = taken     # an unused slot
    case(f"generated name listed ({'Mew' if card == MEW else 'Venusaur'})", "TCG",
         seeded, [key], ["DONE."], [card])
sys.exit(1 if fail else 0)
