#!/usr/bin/env python3
"""Saved games for the crew's rigs (docs/privateer.md section 30), made from one saved game by bytes.

  crewsave.py orion  <in.SAV> <game-written Orion PLAYTYPE.IFF> <ORIOTYPE.IFF> <out.SAV>
      the saved game's pilot in an Orion: the ship file the game itself wrote for an Orion bought at the dealer with its
      Tail Turret mounted (PLAYTYPE.IFF after that launch), given guns on the type's own mounts (ORIOTYPE.IFF's: two in
      front, two in the turret -- lasers) and the type's shields and armour (a bought ship comes bare)
  crewsave.py galaxy <in.SAV> <out.SAV>      the Centurion's saved game as a Galaxy, both turrets armed (same sizes)
  crewsave.py mixed  <in.SAV> <out.SAV>      its tail turret a mass driver and a tachyon cannon

A saved game: u32 its size, then a table of u32 offsets (top byte E0) of its records -- 0: 9 bytes whose first is the
hull (0 Tarsus, 1 Orion, 2 Centurion, 3 Galaxy) and third the base; then the score, the missions, the sectors, the
ship (an IFF FORM REAL, PLAYTYPE.IFF's bytes), the name, the callsign.
"""
import struct, sys

def chunks(d, off, end):
    while off + 8 <= end:
        tag, size = d[off:off + 4], struct.unpack(">I", d[off + 4:off + 8])[0]
        yield tag, off, size
        off += 8 + size + (size & 1)

def form(kind, body):
    return b"FORM" + struct.pack(">I", len(body) + 4) + kind + body

def chunk(tag, body):
    return tag + struct.pack(">I", len(body)) + body + (b"\0" if len(body) & 1 else b"")

def find_form(d, kind):
    """The first FORM of that kind anywhere inside d: (offset, whole length)."""
    i = d.find(b"FORM")
    while i >= 0:
        if d[i + 8:i + 12] == kind:
            return i, 8 + struct.unpack(">I", d[i + 4:i + 8])[0]
        i = d.find(b"FORM", i + 4)
    raise SystemExit("no FORM %s" % kind.decode())

def table(sav):
    first = struct.unpack("<I", sav[4:8])[0] & 0xFFFFFF
    return [struct.unpack("<I", sav[4 + 4 * i:8 + 4 * i])[0] & 0xFFFFFF for i in range((first - 4) // 4)]

def replace_ship(sav, real):
    offs = table(sav)
    k = next(i for i, o in enumerate(offs) if sav[o:o + 4] == b"FORM" and sav[o + 8:o + 12] == b"REAL")
    end = offs[k + 1]
    out = bytearray(sav[:offs[k]] + real + sav[end:])
    delta = len(real) - (end - offs[k])
    for i in range(k + 1, len(offs)):
        struct.pack_into("<I", out, 4 + 4 * i, 0xE0000000 | (offs[i] + delta))
    struct.pack_into("<I", out, 0, len(out))
    return out

def main(a):
    if a[0] == "orion":
        sav, ship, typ = (open(p, "rb").read() for p in a[1:4])
        fite_at, fite_len = find_form(ship, b"FITE")
        tw_at, tw_len = find_form(typ, b"WEAP")
        guns = next(typ[o + 8:o + 8 + s] for t, o, s in chunks(typ, tw_at + 12, tw_at + tw_len) if t == b"GUNS")
        mounts = [guns[i + 1] for i in range(0, len(guns), 4)]
        weap = form(b"WEAP", chunk(b"GUNS", b"".join(bytes([5, m, 0, 0]) for m in mounts)))      # (5: the laser)
        ts_at, ts_len = find_form(typ, b"SHLD")
        body = b""
        for t, o, s in chunks(ship, fite_at + 12, fite_at + fite_len):
            whole = ship[o:o + 8 + s + (s & 1)]
            kind = ship[o + 8:o + 12] if t == b"FORM" else b""
            body += weap if kind == b"WEAP" else typ[ts_at:ts_at + ts_len] if kind == b"SHLD" else whole
        out = replace_ship(sav, form(b"REAL", form(b"FITE", body)))
        out[table(out)[0]] = 1
        print("an Orion: guns on mounts %s, %d bytes" % (mounts, len(out)))
        open(a[4], "wb").write(out)
        return
    sav = bytearray(open(a[1], "rb").read())
    real = table(sav)[6]
    g = sav.find(b"GUNS", real)
    if a[0] == "galaxy":
        sav[g + 8:g + 32] = bytes([0, 5, 0, 0, 0, 7, 0, 0, 7, 8, 0, 0, 7, 10, 0, 0, 2, 2, 0, 0, 2, 3, 0, 0])
        t = sav.find(b"TRRT", real)
        sav[t + 4:t + 10] = bytes([0, 0, 0, 2, 2, 3])
        sav[table(sav)[0]] = 3
    elif a[0] == "mixed":
        sav[g + 8:g + 16] = bytes([3, 7, 0, 0, 7, 5, 0, 0])
    open(a[2], "wb").write(sav)

if __name__ == "__main__":
    main(sys.argv[1:])
