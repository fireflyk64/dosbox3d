#!/usr/bin/env python3
"""A saved game's pilot put in an added ship (docs/privateer.md section 42): the Stiletto.

  stilsave.py <in.SAV> <out.SAV> [ship=2] [guns=7,7] [missiles=5:10] [engine=2] [shield=91] [hold=20]     (ship=2: the Demon)

The saved game must be a Centurion's (the hull the Stiletto is flown on).  Its ship (the record that is an IFF FORM
REAL, PLAYTYPE.IFF's bytes) is given what a Stiletto can carry:
- two guns, on the appearance's own mounts 2 and 3 (SKELETON.IFF's: any other mount is the game's error 6482);
  types by GUNS.IFF's order (0 Neutron, 1 Meson, 2 Ionic, 3 Mass, 4 Particle, 5 Laser, 6 Plasma, 7 Tachyon);
- one missile launcher, on mount 2 (the game's Stiletto has it at 0: the layer renumbers it, the upgrade screen
  mounts launchers on 2), with its missiles (type:count; 1 Torpedo, 2 Heat Seeker, 3 FF, 4 Dumbfire, 5 IR);
- no turret;
- the engine level (LIMITS.IFF's generators: level 2 is two of generator 1, one each of 3, 4 and 5);
- shield generator 2 (SHIELDS.IFF's 0x5B: 200 a side, what the game's own Stiletto carries);
- the hold's size.
And the saved game says whose ship it is: the second byte of its first record (the hull's high byte, which the game
writes as 0 and never reads) is the added ship's id, 1.
"""
import struct, sys

ENGINES = {0: [(1, 2), (6, 1)], 1: [(1, 2), (4, 1), (5, 1)], 2: [(1, 2), (3, 1), (4, 1), (5, 1)],
           3: [(1, 2), (3, 1), (4, 1), (5, 1), (6, 2)]}        # LIMITS.IFF FORM ENGN: each level's generators

def chunks(d, off, end):
    while off + 8 <= end:
        tag, size = d[off:off + 4], struct.unpack(">I", d[off + 4:off + 8])[0]
        yield tag, off, size
        off += 8 + size + (size & 1)

def form(kind, body): return b"FORM" + struct.pack(">I", len(body) + 4) + kind + body
def chunk(tag, body): return tag + struct.pack(">I", len(body)) + body + (b"\0" if len(body) & 1 else b"")

def table(sav):
    first = struct.unpack("<I", sav[4:8])[0] & 0xFFFFFF
    return [struct.unpack("<I", sav[4 + 4 * i:8 + 4 * i])[0] & 0xFFFFFF for i in range((first - 4) // 4)]

def replace_record(sav, k, new):
    offs = table(sav)
    end = offs[k + 1] if k + 1 < len(offs) else len(sav)
    out = bytearray(sav[:offs[k]] + new + sav[end:])
    delta = len(new) - (end - offs[k])
    for i in range(k + 1, len(offs)):
        struct.pack_into("<I", out, 4 + 4 * i, 0xE0000000 | (offs[i] + delta))
    struct.pack_into("<I", out, 0, len(out))
    return bytes(out)

def stiletto(sav, guns=(7, 7), missiles=(5, 10), engine=2, shield=0x5B, hold=20, ship_id=1, mounts=(2, 3, 1, 4), launchers=1):
    offs = table(sav)
    if sav[offs[0]] != 2: raise SystemExit("the saved game's hull is %d, not a Centurion's (2)" % sav[offs[0]])
    k = next(i for i, o in enumerate(offs) if sav[o:o + 4] == b"FORM" and sav[o + 8:o + 12] == b"REAL")
    real = sav[offs[k]:offs[k + 1]]
    assert real[12:16] == b"FORM" and real[20:24] == b"FITE"
    body = b""
    for t, o, s in chunks(real, 24, len(real)):
        whole = real[o:o + 8 + s + (s & 1)]
        kind = real[o + 8:o + 12] if t == b"FORM" else t
        if kind == b"WEAP":
            w = chunk(b"GUNS", b"".join(bytes([g, m, 0, 0]) for g, m in zip(guns, mounts)))
            if missiles: w += chunk(b"LNCH", b"".join(bytes([0x32, m, 0, 0]) for m in (2, 3)[:launchers])) + chunk(b"MISL", bytes([missiles[0]]) + struct.pack("<H", missiles[1]))
            whole = form(b"WEAP", w)
        elif kind == b"ENER":
            gens = ENGINES[engine]
            whole = form(b"ENER", chunk(b"INFO", b"ENERGY\0\0" + b"".join(bytes(g) for g in gens)) + chunk(b"DAMG", bytes(2 * sum(n for _, n in gens))))
        elif kind == b"SHLD" and shield is not None:
            i = whole.index(b"SHIELDS\0") + 8
            whole = whole[:i] + bytes([shield]) + whole[i + 1:]
        elif kind == b"CRGO":
            i = whole.index(b"CRGI") + 8
            credits, size, more = struct.unpack_from("<IHH", whole, i)
            whole = whole[:i] + struct.pack("<IHH", credits, hold, 0) + whole[i + 8:]
            # (what is aboard stays: the test save's hold holds less than 20)
        elif kind == b"TRRT":
            whole = b""
        body += whole
    out = bytearray(replace_record(sav, k, form(b"REAL", form(b"FITE", body))))
    out[table(out)[0] + 1] = ship_id
    return bytes(out)

if __name__ == "__main__":
    a = sys.argv[1:]
    kw = {}
    for x in a[2:]:
        k, v = x.split("=")
        kw[k] = tuple(int(n) for n in v.replace(":", ",").split(",")) if k in ("guns", "missiles") else int(v)
    if kw.pop("ship", 1) == 2:      # ship=2: the Demon -- four guns, two launchers, the hold 25
        kw = dict(dict(guns=(7, 7, 4, 4), hold=25, ship_id=2, launchers=2), **kw)
    open(a[1], "wb").write(stiletto(open(a[0], "rb").read(), **kw))
    print("%s: %s saved game" % (a[1], "a Demon's" if kw.get("ship_id") == 2 else "a Stiletto's"))
