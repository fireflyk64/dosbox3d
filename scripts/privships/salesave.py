#!/usr/bin/env python3
"""Saved games that stand where an added ship is sold (docs/privateer.md section 42), made from another saved game.

  salesave.py <in.SAV> <out.SAV> terrell|tucks [credits=900000]

terrell: at Perry Naval Base with the campaign won -- the plot's mission is the last entry ("FFFFFFFF") and the
         rooms' flag 0x0B is set (the Admiral's door opens; talking to him is his "one more thing", the credits,
         and the Stiletto's offer).  From the pad: the door at the left, then the office (a small spot at 0.54,
         0.57 of the concourse), then the Admiral.
tucks:   at Tuck's, the pirate base of the Sherwood system: the pad's exit, the bar (the concourse's right, 0.93,
         0.67), the bartender (his rumour, then the Demon's offer).

The saved game's layout (its records by the table at its head): record 0 the hull, the base (+2), ...; record 1 the
plot's mission (8 letters); record 4 the rooms' flags from +0x0C; the ship's FORM REAL with FORM CRGO: CRGI = the
credits (a dword), the hold; DATA = what is aboard (4 bytes a kind: the kind, the amount as a word, a byte).  What is
aboard is cut to 16 units so that it fits either ship's hold.
"""
import struct, sys

def table(sav):
    first = struct.unpack_from("<I", sav, 4)[0] & 0xFFFFFF
    return [struct.unpack_from("<I", sav, 4 + 4 * i)[0] & 0xFFFFFF for i in range((first - 4) // 4)]

def make(sav, where, credits=900000):
    d = bytearray(sav); offs = table(d)
    if where == "terrell":
        d[offs[0] + 2] = 0x29
        d[offs[1]:offs[1] + 8] = b"FFFFFFFF"
        d[offs[4] + 0x0C + 0x0B] = 1
    elif where == "tucks":
        d[offs[0] + 2] = 0x36
    else:
        raise SystemExit("terrell or tucks")
    i = d.index(b"CRGI"); struct.pack_into("<I", d, i + 8, credits)
    j = d.index(b"DATA", d.index(b"CRGO")); n = struct.unpack_from(">I", d, j + 4)[0]
    room = 16
    for k in range(j + 8, j + 8 + n, 4):
        have = struct.unpack_from("<H", d, k + 1)[0]
        struct.pack_into("<H", d, k + 1, min(have, room)); room -= min(have, room)
    return bytes(d)

if __name__ == "__main__":
    a = sys.argv[1:]
    kw = dict(x.split("=") for x in a[3:])
    open(a[1], "wb").write(make(open(a[0], "rb").read(), a[2], int(kw.get("credits", 900000))))
    print("%s: at %s, %s credits" % (a[1], "Perry Naval Base, the campaign won" if a[2] == "terrell" else "Tuck's (Sherwood)", kw.get("credits", 900000)))
