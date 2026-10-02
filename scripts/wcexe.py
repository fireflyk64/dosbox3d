#!/usr/bin/env python3
"""wcexe.py -- disassembly of the Wing Commander executables (WC.EXE, SM2.EXE,
WC2.EXE, SO1.EXE, SO2.EXE) from the executable alone.

All of them are Borland C++ 1991 overlay (VROOMM) programs.  After the MZ
image comes an 'FBOV' block: the overlay code, and the offset and length of
the linker's segment table, which sits inside the root image.  Each table
entry is 8 bytes (paragraph, a size word, flags, a start offset); the index of
an entry is what overlay code uses for a far call's segment operand, times 8
(the overlay loader patches the real segment in).  A code entry whose segment
starts with `int 3F` is an overlay's stub: a 0x20 byte header (file offset and
size of the overlay's code) and one 5 byte thunk per exported function.

Address forms, the same as docs/wcnet-multiplayer.md uses for WC.EXE:

  seg001:20E3     root image, segment index 1 (fixed after load)
  stub143:0084    thunk in overlay 143's stub
  ovr143:0A99     offset inside overlay 143's code (loaded where memory allows)
  dseg:C430       the data segment (DGROUP)

  scripts/wcexe.py [-e wc2/WC2.EXE] segs
  scripts/wcexe.py dis ovr143:0A99 [length]     # default: to the function's end
  scripts/wcexe.py entries stub143
  scripts/wcexe.py xref ovr143:0A99             # far, near and root callers
  scripts/wcexe.py find 1abd0900 ...            # byte patterns (hex)
  scripts/wcexe.py str "Shield gen"             # where a string is, and who names it
  scripts/wcexe.py funcs ovr143                 # function starts in a segment
  scripts/wcexe.py match wc/WC.EXE ovr143:0A99  # the most similar functions in -e's executable

Names: scripts/<exe basename>.names, lines of "<address> <name> [; comment]",
are shown in listings and accepted as addresses.

The executable defaults to $WC_EXE or wc/WC.EXE.  Needs ndisasm.
"""
import bisect
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Exe(object):
    def __init__(self, path):
        self.path = path
        self.data = d = open(path, "rb").read()
        assert d[:2] == b"MZ", "%s is not an MZ executable" % path
        self.hdr = struct.unpack_from("<H", d, 8)[0] * 16
        self.entry = struct.unpack_from("<HH", d, 0x14)  # ip, cs
        self.fbov = d.find(b"FBOV")
        assert self.fbov > 0, "no Borland overlay block in %s" % path
        _sig, self.ovr_size, table, count = struct.unpack_from("<4sIII", d, self.fbov)
        self.ovr_base = self.fbov + 16
        # segment table
        self.segs = []  # dicts: idx, para, name, kind, start (image offset), end
        for i in range(count):
            para, size, flags, minoff = struct.unpack_from("<HHHH", d, table + 8 * i)
            self.segs.append(dict(idx=i, para=para, size=size, flags=flags, minoff=minoff))
        image_end = self.fbov - self.hdr
        for i, s in enumerate(self.segs):
            nxt = [t["para"] for t in self.segs[i + 1:] if t["para"] > s["para"]]
            s["start"] = s["para"] * 16
            s["end"] = min(nxt[0] * 16 if nxt else image_end, image_end)
            # flags: 1 code, 3 an overlay's stub, 0 and 4 data
            is_stub = s["flags"] == 3 and d[self.hdr + s["start"]:self.hdr + s["start"] + 2] == b"\xcd\x3f"
            s["kind"] = "stub" if is_stub else ("code" if s["flags"] == 1 else "data")
            s["name"] = ("stub%d" if is_stub else "seg%03d") % i
        # DGROUP: Borland's startup begins with mov dx, seg DGROUP
        e = self.hdr + self.entry[1] * 16 + self.entry[0]
        assert d[e] == 0xBA, "unexpected startup code"
        self.dgroup = struct.unpack_from("<H", d, e + 1)[0]
        self.by_name = dict((s["name"], s) for s in self.segs)
        self.by_para = {}
        for s in self.segs:
            self.by_para.setdefault(s["para"], s)
        self.by_name["dseg"] = dict(idx=-1, para=self.dgroup, start=self.dgroup * 16,
                                    end=min(self.dgroup * 16 + 0x10000, image_end), kind="data", name="dseg")
        # overlays
        self.ovr = {}
        for s in self.segs:
            if s["kind"] != "stub":
                continue
            off = self.hdr + s["start"]
            _magic, _swap, fileofs, codesize, relsize, nentries, _prev = struct.unpack_from("<HHIHHHH", d, off)
            entries = []
            for k in range(nentries):
                magic, target, _ = struct.unpack_from("<HHB", d, off + 0x20 + 5 * k)
                if magic != 0x3FCD:
                    break
                entries.append((0x20 + 5 * k, target))
            self.ovr[s["idx"]] = dict(file=self.ovr_base + fileofs, size=codesize, relsize=relsize, entries=entries)
            self.by_name["ovr%d" % s["idx"]] = dict(idx=s["idx"], name="ovr%d" % s["idx"], kind="ovr", size=codesize)
        self.names, self.name_addr, self.comments = {}, {}, {}
        base = os.path.splitext(os.path.basename(path))[0].lower()
        names_file = os.path.join(ROOT, "scripts", base + ".names")
        if os.path.exists(names_file):
            for line in open(names_file):
                line = line.split("#")[0].rstrip()
                m = re.match(r"^(\S+)\s+(\S+)(?:\s*;\s*(.*))?$", line)
                if m:
                    self.names[m.group(1)] = m.group(2)
                    self.name_addr[m.group(2)] = m.group(1)
                    if m.group(3):
                        self.comments[m.group(1)] = m.group(3)
        self._funcs = {}

    # ---- addresses -----------------------------------------------------
    def parse(self, text):
        """-> (segment name, offset)"""
        text = self.name_addr.get(text, text)
        m = re.match(r"^(\w+):([0-9A-Fa-f]+)$", text)
        assert m and m.group(1) in self.by_name, "unknown address %s" % text
        return m.group(1), int(m.group(2), 16)

    def fmt(self, seg, off):
        return "%s:%04X" % (seg, off)

    def label(self, seg, off):
        a = self.fmt(seg, off)
        return "%s <%s>" % (a, self.names[a]) if a in self.names else a

    def seg_bytes(self, seg):
        """-> (bytes of the whole segment, offset of its first byte)"""
        s = self.by_name[seg]
        if s["kind"] == "ovr":
            o = self.ovr[s["idx"]]
            return self.data[o["file"]:o["file"] + o["size"]], 0
        # a root segment's code begins at its paragraph; offsets count from there
        return self.data[self.hdr + s["start"]:self.hdr + s["end"]], 0

    def file_to_addr(self, f):
        if f >= self.ovr_base:
            for n, o in self.ovr.items():
                if o["file"] <= f < o["file"] + o["size"]:
                    return "ovr%d" % n, f - o["file"]
            return None
        img = f - self.hdr
        best = None
        for s in self.segs:
            if s["start"] <= img < s["end"] and (best is None or s["start"] > best["start"]):
                best = s
        if best is None:
            return None
        if self.dgroup * 16 <= img < self.dgroup * 16 + 0x10000 and best["kind"] == "data":
            return "dseg", img - self.dgroup * 16
        return best["name"], img - best["start"]

    def stub_target(self, n, stuboff):
        for so, target in self.ovr[n]["entries"]:
            if so == stuboff:
                return target
        return None

    def far_target(self, from_seg, segv, offv):
        """Resolve a far operand to (segment name, offset) or None."""
        if from_seg.startswith("ovr"):
            if segv % 8 or segv // 8 >= len(self.segs):
                return None
            s = self.segs[segv // 8]
        else:
            s = self.by_para.get(segv)
            if s is None:
                return None
        return s["name"], offv

    # ---- disassembly -----------------------------------------------------
    def ndisasm(self, code, origin):
        p = subprocess.run(["ndisasm", "-b", "16", "-o", "0x%x" % origin, "-"], input=code, capture_output=True)
        out = []
        for line in p.stdout.decode().splitlines():
            m = re.match(r"^([0-9A-F]+)\s+([0-9A-F]+)\s+(.*)$", line)
            if m:
                out.append((int(m.group(1), 16), m.group(2), m.group(3)))
            elif out and re.match(r"^\s+-([0-9A-F]+)$", line):
                out[-1] = (out[-1][0], out[-1][1] + line.strip()[1:], out[-1][2])
        return out

    def annotate(self, seg, text):
        def far(m):
            t = self.far_target(seg, int(m.group(2), 16), int(m.group(3), 16))
            if t is None:
                return m.group(0)
            name, off = t
            if name.startswith("stub"):
                tgt = self.stub_target(int(name[4:]), off)
                if tgt is not None:
                    return "%s %s -> %s" % (m.group(1), self.fmt(name, off), self.label("ovr" + name[4:], tgt))
            return "%s %s" % (m.group(1), self.label(name, off))
        text = re.sub(r"\b(shl|shr|sar|sal|rol|ror|rcl|rcr) (\w+),0x0$", r"\1 \2,1", text)
        text = re.sub(r"(call|jmp)\s+(?:word\s+)?(0x[0-9a-f]+):(?:word\s+)?(0x[0-9a-f]+)", far, text)

        def near(m):
            return "%s %s" % (m.group(1), self.label(seg, int(m.group(2), 16)))
        text = re.sub(r"\b(call|jmp short|jmp|j[a-z]+|loop)\s+(0x[0-9a-f]+)$", near, text)
        # [bx-0x42e6] is dseg:BD1A + bx: show the unsigned offset
        def disp(m):
            v = (0x10000 - int(m.group(2), 16)) & 0xFFFF
            a = "dseg:%04X" % v
            return "[%s+0x%x%s]" % (m.group(1), v, "=" + self.names[a] if a in self.names else "")
        text = re.sub(r"\[(bx|si|di|bx\+si|bx\+di)-(0x[0-9a-f]{3,4})\]", disp, text)

        def absolute(m):
            a = "dseg:%04X" % int(m.group(1), 16)
            return "[0x%x=%s]" % (int(m.group(1), 16), self.names[a]) if a in self.names else m.group(0)
        text = re.sub(r"\[(0x[0-9a-f]+)\]", absolute, text)
        return text

    def disasm(self, seg, off, length, raw=False):
        code, base = self.seg_bytes(seg)
        code = code[off - base:off - base + length]
        out = []
        for o, hexb, text in self.ndisasm(code, off):
            a = self.fmt(seg, o)
            if a in self.names:
                out.append("")
                out.append("%s  %s:%s" % (a, self.names[a], "   ; " + self.comments[a] if a in self.comments else ""))
            out.append("%s  %s%s" % (a, "" if raw else "", self.annotate(seg, text)))
        return "\n".join(out)

    # ---- functions -------------------------------------------------------
    def funcs(self, seg):
        """Function starts in a code segment: Borland prologues (push bp /
        mov bp,sp, with inc bp before it in overlay-aware far functions) and
        the overlay's exported entries."""
        if seg in self._funcs:
            return self._funcs[seg]
        code, base = self.seg_bytes(seg)
        starts = set()
        i = code.find(b"\x55\x8b\xec")
        while i >= 0:
            starts.add(i - 1 if i > 0 and code[i - 1] == 0x45 else i)
            i = code.find(b"\x55\x8b\xec", i + 1)
        s = self.by_name[seg]
        if s["kind"] == "ovr":
            for _so, target in self.ovr[s["idx"]]["entries"]:
                starts.add(target)
        out = sorted(starts)
        self._funcs[seg] = out
        return out

    def func_bounds(self, seg, off):
        starts = self.funcs(seg)
        code, _ = self.seg_bytes(seg)
        i = bisect.bisect_right(starts, off)
        start = starts[i - 1] if i > 0 else off
        end = starts[i] if i < len(starts) else len(code)
        return start, end

    def code_segments(self):
        out = [s["name"] for s in self.segs if s["kind"] == "code"]
        out += ["ovr%d" % n for n in sorted(self.ovr)]
        return out

    # ---- searching -------------------------------------------------------
    def find(self, pat):
        out, i = [], self.data.find(pat)
        while i >= 0:
            a = self.file_to_addr(i)
            out.append(self.fmt(*a) if a else "file+0x%x" % i)
            i = self.data.find(pat, i + 1)
        return out

    def xref(self, seg, off):
        """Callers of seg:off: far calls (root and overlay encodings, through
        the stub for an overlay function) and near calls in its own segment."""
        out = []
        targets = []  # (segment table entry, offset)
        s = self.by_name[seg]
        if s["kind"] == "ovr":
            st = self.by_name["stub%d" % s["idx"]]
            for so, target in self.ovr[s["idx"]]["entries"]:
                if target == off:
                    targets.append((st, so))
        elif s["idx"] >= 0:
            targets.append((s, off))
        for t, toff in targets:
            for op, what in ((0x9A, "call"), (0xEA, "jmp")):
                for segword, where in ((t["para"], "root"), (t["idx"] * 8, "ovr")):
                    pat = bytes([op]) + struct.pack("<HH", toff, segword)
                    i = self.data.find(pat)
                    while i >= 0:
                        a = self.file_to_addr(i)
                        if a and (a[0].startswith("ovr") == (where == "ovr")):
                            out.append("%s  far %s%s" % (self.label(*a), what, " via %s" % self.fmt(t["name"], toff) if t is not s else ""))
                        i = self.data.find(pat, i + 1)
        code, _ = self.seg_bytes(seg) if s["kind"] in ("ovr", "code") else (b"", 0)
        for i in range(len(code) - 2):
            if code[i] == 0xE8 and (i + 3 + struct.unpack_from("<h", code, i + 1)[0]) & 0xFFFF == off:
                out.append("%s  near call" % self.label(seg, i))
        return out

    def data_refs(self, off):
        """Code that names dseg:off as a 16-bit displacement or immediate."""
        pat = struct.pack("<H", off)
        out = []
        for seg in self.code_segments():
            code, _ = self.seg_bytes(seg)
            i = code.find(pat)
            while i >= 0:
                out.append(self.fmt(seg, i))
                i = code.find(pat, i + 1)
        return out

    # ---- similarity ------------------------------------------------------
    def shape(self, seg, start, end):
        """A function as a list of tokens that survive relinking: mnemonics
        and register operands, with addresses and far targets blanked and
        small constants kept."""
        code, _ = self.seg_bytes(seg)
        toks = []
        for _o, _h, text in self.ndisasm(code[start:end], start):
            text = re.sub(r"0x[0-9a-f]+:(?:word )?0x[0-9a-f]+|word 0x[0-9a-f]+:word 0x[0-9a-f]+", "FAR", text)
            text = re.sub(r"\[[^\]]*0x[0-9a-f]{3,}[^\]]*\]", "[M]", text)       # data addresses
            text = re.sub(r"\b(call|jmp short|jmp|j[a-z]+|loop)\s+0x[0-9a-f]+", r"\1 L", text)
            text = re.sub(r"0x[0-9a-f]{3,}", "N", text)                         # big constants move too
            toks.append(text)
        return toks


def similarity(a, b):
    """Dice coefficient over bigrams of instruction tokens."""
    def grams(t):
        g = {}
        for i in range(len(t) - 1):
            k = (t[i], t[i + 1])
            g[k] = g.get(k, 0) + 1
        return g
    ga, gb = grams(a), grams(b)
    if not ga or not gb:
        return 0.0
    common = sum(min(n, gb.get(k, 0)) for k, n in ga.items())
    return 2.0 * common / (sum(ga.values()) + sum(gb.values()))


def main(argv):
    exe = os.environ.get("WC_EXE", os.path.join(ROOT, "wc", "WC.EXE"))
    args = argv[1:]
    if args[:1] == ["-e"]:
        exe, args = args[1], args[2:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    x = Exe(exe)
    cmd = args[0]
    if cmd == "segs":
        print("%s: %d segments, DGROUP paragraph %04X, %d overlays" % (exe, len(x.segs), x.dgroup, len(x.ovr)))
        for s in x.segs:
            extra = ""
            if s["kind"] == "stub":
                o = x.ovr[s["idx"]]
                extra = "  overlay code 0x%X bytes at file 0x%X, %d entries" % (o["size"], o["file"], len(o["entries"]))
            print("%-8s para %04X  %05X..%05X  %s%s%s" % (s["name"], s["para"], s["start"], s["end"], s["kind"],
                                                         "  (DGROUP)" if s["para"] == x.dgroup else "", extra))
    elif cmd == "dis":
        seg, off = x.parse(args[1])
        if seg.startswith("stub"):
            seg, off = "ovr" + seg[4:], x.stub_target(int(seg[4:]), off)
        if len(args) > 2:
            length = int(args[2], 16)
        else:
            _s, end = x.func_bounds(seg, off)
            length = end - off
        print("; %s  length 0x%X" % (x.label(seg, off), length))
        print(x.disasm(seg, off, length))
    elif cmd == "entries":
        n = int(re.sub(r"\D", "", args[1]))
        o = x.ovr[n]
        print("stub%d: overlay code at file 0x%X, 0x%X bytes" % (n, o["file"], o["size"]))
        for so, target in o["entries"]:
            print("stub%d:%04X -> %s" % (n, so, x.label("ovr%d" % n, target)))
    elif cmd == "xref":
        for a in args[1:]:
            seg, off = x.parse(a)
            print("%s:" % x.label(seg, off))
            for line in x.xref(seg, off):
                print("   " + line)
    elif cmd == "refs":
        for a in args[1:]:
            seg, off = x.parse(a if ":" in a else "dseg:" + a)
            print("%s: %s" % (x.fmt(seg, off), " ".join(x.data_refs(off))))
    elif cmd == "find":
        for h in args[1:]:
            print(h, " ".join(x.find(bytes.fromhex(h))))
    elif cmd == "str":
        for text in args[1:]:
            for a in x.find(text.encode("latin-1")):
                line = a
                if a.startswith("dseg:"):
                    off = int(a[5:], 16)
                    line += "  named by: " + " ".join(x.data_refs(off)[:12])
                print(line)
    elif cmd == "funcs":
        seg = args[1]
        starts = x.funcs(seg)
        code, _ = x.seg_bytes(seg)
        exported = set(t for _so, t in x.ovr[x.by_name[seg]["idx"]]["entries"]) if seg.startswith("ovr") else set()
        for i, s in enumerate(starts):
            end = starts[i + 1] if i + 1 < len(starts) else len(code)
            print("%s  0x%04X bytes%s" % (x.label(seg, s), end - s, "  exported" if s in exported else ""))
    elif cmd == "match":
        other = Exe(args[1])
        for a in args[2:]:
            seg, off = other.parse(a)
            start, end = other.func_bounds(seg, off)
            want = other.shape(seg, start, end)
            scored = []
            for cseg in x.code_segments():
                starts = x.funcs(cseg)
                code, _ = x.seg_bytes(cseg)
                for i, s in enumerate(starts):
                    e = starts[i + 1] if i + 1 < len(starts) else len(code)
                    if not (0.5 * (end - start) <= e - s <= 2.0 * (end - start) + 16):
                        continue
                    scored.append((similarity(want, x.shape(cseg, s, e)), cseg, s, e - s))
            scored.sort(reverse=True)
            print("%s (%d bytes, %d instructions) in %s:" % (other.label(seg, start), end - start, len(want), os.path.basename(exe)))
            for score, cseg, s, size in scored[:5]:
                print("   %.2f  %s  0x%X bytes" % (score, x.label(cseg, s), size))
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
