#!/usr/bin/env python3
"""
test_patcher.py — headless test of the save patcher on fake carts.

`make tests` builds the patcher with Crystal (BYTE / BYTD / BXTJ), Gold/Silver
(AAXD / AAUE / AAUJ) and Red / Yellow / Japanese-Red headers. The patcher then
checks and patches its *own* cartridge, so no cart swap is needed. Each case
seeds the cart's SRAM, presses START, checks the menu, presses a button, reads
the result screen and SRAM back, checks the return to the prompt, and runs the
same button once more (the event: "already"; a Pokémon: added again, or "party
full"). Finally a party of 3 gets three adds (slots 4-6) and one "party full".

Needs PyBoy (`pip install pyboy`). Run: make tests && ./test_patcher.py
"""

import io
import os
import re
import sys
from pyboy import PyBoy

HERE = os.path.dirname(os.path.abspath(__file__))
B1 = 0x2000                                  # .sav offset of SRAM bank 1

# Crystal event (pokecrystal sram.asm)
IDX, IDX_BAK = B1 + 0x1E3C, B1 + 0x1E44      # $BE3C / $BE44
CV1, C_CV2 = B1 + 0x0008, B1 + 0x0D0F        # $A008 / $AD0F
BCV1, BCV2 = 0x1208, 0x1F0F                  # bank 0 $B208 / $BF0F

# party layouts: party count, struct length, dex owned/seen, checksummed range
LAYOUT = {
    "gen1": dict(party=B1 + 0x0F2C, slen=44, owned=B1 + 0x05A3, seen=B1 + 0x05B6,
                 sum_from=B1 + 0x0598, sum_end=B1 + 0x1523, ck=B1 + 0x1523),
    "gs":   dict(party=B1 + 0x088A, slen=48, owned=B1 + 0x0A4C, seen=B1 + 0x0A6C,
                 sum_from=B1 + 0x0009, sum_end=B1 + 0x0D69, ck=B1 + 0x0D69, cv2=B1 + 0x0D6B),
    "c":    dict(party=B1 + 0x0865, slen=48, owned=B1 + 0x0A27, seen=B1 + 0x0A47,
                 sum_from=B1 + 0x0009, sum_end=B1 + 0x0B83, ck=B1 + 0x0D0D, cv2=C_CV2),
}


def load_mon(name):
    inc = open(os.path.join(HERE, name)).read()
    data = bytes(int(x, 16) for x in re.findall(r"\$([0-9A-F]{2})", inc.split("MACRO")[1]))
    return data, int(re.search(r"_DEX EQU (\d+)", inc)[1])


MEW, MEW_DEX = load_mon("mew.inc")
MEW2, _ = load_mon("mew2.inc")
CELEBI, CELEBI_DEX = load_mon("celebi.inc")
assert len(MEW) == 66 and len(MEW2) == len(CELEBI) == 70


def seal(s, g):
    L = LAYOUT[g]
    total = sum(s[L["sum_from"]:L["sum_end"]])
    if g == "gen1":
        s[L["ck"]] = ~total & 0xFF
    else:
        s[L["ck"]], s[L["ck"] + 1] = total & 0xFF, total >> 8 & 0xFF


def save(g, species=(0x54,), bad_checksum=False, event=0, main=True, backup=False):
    s = bytearray(0x8000)
    L = LAYOUT[g]
    if g != "gen1" and main:
        s[CV1], s[L["cv2"]] = 99, 127
    if backup:
        s[BCV1], s[BCV2] = 99, 127
    s[IDX] = s[IDX_BAK] = event
    p = L["party"]
    s[p] = len(species)
    s[p + 1:p + 8] = bytes(species) + b"\xFF" * (7 - len(species))
    for i, sp in enumerate(species):
        s[p + 8 + L["slen"] * i] = sp
    seal(s, g)
    if bad_checksum:
        s[L["ck"]] ^= 1
    return s


def added(g, mon, dex):
    def check(before, after):
        L = LAYOUT[g]
        s = bytearray(before)
        p, n, sl = L["party"], before[L["party"]], L["slen"]
        s[p] = n + 1
        s[p + 1 + n], s[p + 2 + n] = mon[0], 0xFF
        mons = p + 8
        ots = mons + 6 * sl
        nicks = ots + 66
        s[mons + sl * n:mons + sl * n + sl] = mon[:sl]
        s[ots + 11 * n:ots + 11 * n + 11] = mon[sl:sl + 11]
        s[nicks + 11 * n:nicks + 11 * n + 11] = mon[sl + 11:]
        s[L["owned"] + (dex - 1) // 8] |= 1 << ((dex - 1) % 8)
        s[L["seen"] + (dex - 1) // 8] |= 1 << ((dex - 1) % 8)
        seal(s, g)
        return s == after
    return check


def event_set(before, after):
    s = bytearray(before)
    s[IDX] = s[IDX_BAK] = 0x0B
    return s == after


def screen_text(pb):
    rows = ("".join(chr(pb.memory[0x9800 + r * 32 + c]) for c in range(20)).rstrip()
            for r in range(18))
    return "\n".join(x for x in rows if x)


def first(text):
    return text.splitlines()[0] if text else ""


def run(code, ram, key, rounds=2):
    pb = PyBoy(os.path.join(HERE, f"test-{code}.gbc"), window="null", cgb=True,
               sound_emulated=False, ram_file=io.BytesIO(bytes(ram)))
    pb.set_emulation_speed(0)
    pb.tick(240)                                 # CGB boot ROM + prompt
    prompt = screen_text(pb)
    out = []
    for _ in range(rounds):
        pb.button("start", 5)
        pb.tick(30)
        menu = screen_text(pb)
        if "SELECT" in menu:
            pb.button(key, 5)
            pb.tick(30)
        result = screen_text(pb)
        pb.memory[0x0000] = 0x0A                 # peek SRAM through the MBC
        sram = bytearray(0x8000)
        for bank in range(4):
            pb.memory[0x4000] = bank
            sram[bank * 0x2000:(bank + 1) * 0x2000] = bytes(pb.memory[0xA000:0xC000])
        pb.memory[0x0000] = 0
        pb.tick(200)                             # > 2.5 s: back at the prompt
        out.append((menu, result, sram, screen_text(pb) == prompt))
    pb.stop(save=False)
    return prompt, out


full = (0x54, 0x99, 0xB0, 0xB1, 0xB2, 0x24)
C_MEW, C_CEL = added("c", MEW2, MEW_DEX), added("c", CELEBI, CELEBI_DEX)
GS_MEW, GS_CEL = added("gs", MEW2, MEW_DEX), added("gs", CELEBI, CELEBI_DEX)
G1_MEW = added("gen1", MEW, MEW_DEX)
CASES = [
    # cart,  sram,                             key,      menu,           screen,          check,  again
    ("BYTE", save("c"),                        "start",  "START: GS",    "DONE.",         event_set, "ALREADY"),
    ("BYTD", save("c", event=0x0B),            "start",  "START: GS",    "ALREADY",       None,   "ALREADY"),
    ("BYTE", save("c", main=False, backup=True, bad_checksum=True),
                                               "start",  "START: GS",    "NO SAVE",       None,   "NO SAVE"),
    ("BYTE", save("c"),                        "b",      "B: ADD CELEBI", "DONE.",        C_CEL,  "DONE."),
    ("BYTD", save("c"),                        "a",      "A: ADD MEW",   "DONE.",         C_MEW,  "DONE."),
    ("BYTD", save("c", full[:5]),              "b",      "B: ADD CELEBI", "DONE.",        C_CEL,  "PARTY FULL"),
    ("BYTE", save("c", full),                  "a",      "A: ADD MEW",   "PARTY FULL",    None,   "PARTY FULL"),
    ("BYTE", save("c", bad_checksum=True),     "b",      "B: ADD CELEBI", "NO SAVE",      None,   "NO SAVE"),
    ("BYTE", save("c"),                        "select", "SELECT: BACK", "SAVE PATCHER",  None,   "SAVE PATCHER"),
    ("BXTJ", save("c"),                        "a",      "NOT SUPPORTED", "NOT SUPPORTED", None,  "NOT SUPPORTED"),
    ("AAXD", save("gs"),                       "b",      "B: ADD CELEBI", "DONE.",        GS_CEL, "DONE."),
    ("AAUE", save("gs"),                       "a",      "A: ADD MEW",   "DONE.",         GS_MEW, "DONE."),
    ("AAUE", save("gs", (0x54, 0xFB)),         "b",      "B: ADD CELEBI", "DONE.",        GS_CEL, "DONE."),
    ("AAXD", save("gs", main=False),           "a",      "A: ADD MEW",   "NO SAVE",       None,   "NO SAVE"),
    ("AAXD", save("gs"),                       "start",  "B: ADD CELEBI", "B: ADD CELEBI", None,  "B: ADD CELEBI"),
    ("AAUJ", save("gs"),                       "a",      "NOT SUPPORTED", "NOT SUPPORTED", None,  "NOT SUPPORTED"),
    ("RED",    save("gen1"),                   "a",      "A: ADD MEW",   "DONE.",         G1_MEW, "DONE."),
    ("YELLOW", save("gen1", ()),               "a",      "A: ADD MEW",   "DONE.",         G1_MEW, "DONE."),
    ("RED",    save("gen1"),                   "b",      "A: ADD MEW",   "A: ADD MEW",    None,   "A: ADD MEW"),
    ("RED",    save("gen1", full),             "a",      "A: ADD MEW",   "PARTY FULL",    None,   "PARTY FULL"),
    ("RED",    save("gen1", (0x54, 0x15)),     "a",      "A: ADD MEW",   "DONE.",         G1_MEW, "DONE."),
    ("RED",    save("gen1", bad_checksum=True), "a",     "A: ADD MEW",   "NO SAVE",       None,   "NO SAVE"),
    ("REDJ",   save("gen1"),                   "a",      "NOT SUPPORTED", "NOT SUPPORTED", None,  "NOT SUPPORTED"),
]

fail = 0
for code, ram, key, want_menu, want, check, want_again in CASES:
    prompt, ((menu, result, sram, back), (_, again, _, back2)) = run(code, ram, key)
    sram_ok = check(ram, sram) if check else ram == sram
    # a button that does not apply is ignored: the menu stays (no return)
    stays = want.startswith(("A: ", "B: "))
    ok = (want_menu in menu and want in result and sram_ok and "PRESS START" in prompt
          and (stays or (back and back2)) and want_again in again)
    fail += not ok
    print(f"{'ok  ' if ok else 'FAIL'} {code:6} {key:6} {first(result)!r:18} "
          f"sram {'ok' if sram_ok else 'WRONG'}, again {first(again)!r}")
    if not ok:
        print("--- menu:\n" + menu + "\n--- result:\n" + result)
# 3 in the party: three adds fill slots 4, 5, 6, the fourth says "party full"
for code, g, key, mon in (("RED", "gen1", "a", MEW), ("BYTD", "c", "b", CELEBI)):
    ram = save(g, (0x54, 0x99, 0xB0))
    _, out = run(code, ram, key, rounds=4)
    L = LAYOUT[g]
    sram = out[3][2]
    p = L["party"]
    ok = ([first(r[1]) for r in out] == ["DONE."] * 3 + ["PARTY FULL"]
          and sram[p] == 6 and list(sram[p + 1:p + 8]) == [0x54, 0x99, 0xB0] + [mon[0]] * 3 + [0xFF]
          and all(sram[p + 8 + L["slen"] * i] == mon[0] for i in (3, 4, 5)))
    fail += not ok
    print(f"{'ok  ' if ok else 'FAIL'} {code:6} {key}x4   slots 4-6 then {first(out[3][1])!r}, "
          f"species {bytes(sram[p + 1:p + 8]).hex()}")
sys.exit(1 if fail else 0)
