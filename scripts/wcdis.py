#!/usr/bin/env python3
"""wcdis.py -- repeatable disassembly of Wing Commander 1 (WC.EXE) with the
symbols from the IDA database export WcMulti/wcmulti.idc.

The multiplayer hooks in src/cpu/wc_net*.cpp are expressed in three address
spaces.  This tool translates between them so that a note like
"ovr143:0A99 do_damage" can be found again in wc.idb, in a DOSBox debugger
session, and in this tool's output.

  ovr143:0A99      overlay-relative offset.  This is what wc.idb / wcmulti.idc
                   name (IDA segment "ovr143").  Overlay code segments are
                   loaded dynamically at run time, so their DOS segment varies.
  stub143:0084     entry in the overlay's stub (jump table).  The DOSBox hooks
                   compare against these: DOS segment = IDA segment + 0x1A2.
                   e.g. stub143 is IDA seg 0x1135 -> DOS seg 0x12D7.
  dseg:C430        data-segment offset (DOS 13D3:C430, IDA seg 0x1231).
  0x2B929          IDA linear address.
  12D7:0084        DOS run-time seg:off (main image + stubs only).

Examples:
  scripts/wcdis.py sym do_damage
  scripts/wcdis.py sym stub143:0084
  scripts/wcdis.py dis do_damage             # until the next named symbol
  scripts/wcdis.py dis ovr143:0A99 0x120
  scripts/wcdis.py callers stub143:0084      # far calls to a stub entry or root function
  scripts/wcdis.py segments
  scripts/wcdis.py entries stub143
  scripts/wcdis.py names ovr143
  scripts/wcdis.py selfcheck

Overlay code encodes far-call segment operands as 8 * segment-index (the
overlay loader patches the real segment in at load time); the root image uses
real paragraphs fixed up by the MZ relocation table.  The tool resolves both.

Paths default to wc/WC.EXE and WcMulti/wcmulti.idc relative to the repo root;
override with WC_EXE / WC_IDC environment variables.
"""
import os
import re
import struct
import subprocess
import sys
import bisect

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.environ.get("WC_EXE", os.path.join(ROOT, "wc", "WC.EXE"))
IDC = os.environ.get("WC_IDC", os.path.join(ROOT, "WcMulti", "wcmulti.idc"))
DOS_SEG_DELTA = 0x1A2      # DOS load segment of the image (seg000 = 01A2)
FBOV_HEADER = 0x10         # bytes of the 'FBOV' header before overlay data


class Image(object):
    def __init__(self, exe=EXE, idc=IDC):
        self.data = open(exe, "rb").read()
        e_cparhdr = struct.unpack_from("<H", self.data, 8)[0]
        self.hdr = e_cparhdr * 16
        self.fbov = self.data.find(b"FBOV")
        assert self.fbov > 0, "no Borland overlay section in %s" % exe
        self.ovr_base = self.fbov + FBOV_HEADER
        self._load_idc(idc)

    # ---- IDC parsing -------------------------------------------------
    def _load_idc(self, path):
        txt = open(path, "rb").read().decode("latin-1")
        self.segs = []  # (start, end, idaseg, name)
        names = {}
        for m in re.finditer(r'SegCreate\(0X([0-9A-F]+),0X([0-9A-F]+),0X([0-9A-F]+),', txt):
            self.segs.append([int(m.group(1), 16), int(m.group(2), 16), int(m.group(3), 16), None])
        for m in re.finditer(r'SegRename\(0X([0-9A-F]+),"([^"]+)"\)', txt):
            names[int(m.group(1), 16)] = m.group(2)
        for s in self.segs:
            s[3] = names.get(s[0], "seg_%X" % s[0])
        self.segs.sort()
        self.seg_by_name = dict((s[3], s) for s in self.segs)
        self.names = {}     # linear -> name
        for m in re.finditer(r'MakeName\s*\(\s*0X([0-9A-F]+)\s*,\s*"([^"]*)"\s*\)', txt):
            self.names[int(m.group(1), 16)] = m.group(2)
        self.comments = {}  # linear -> comment
        for m in re.finditer(r'Make(?:Comm|RptCmt)\s*\(\s*0X([0-9A-F]+)\s*,\s*"((?:[^"\\]|\\.)*)"\s*\)', txt):
            self.comments.setdefault(int(m.group(1), 16), []).append(m.group(2))
        self.name_to_linear = dict((v, k) for k, v in self.names.items())
        self.sorted_names = sorted(self.names)
        self.stubs = {}
        for s in self.segs:
            m = re.match(r"stub(\d+)$", s[3])
            if m:
                self.stubs[int(m.group(1))] = s
        self.dseg = self.seg_by_name["dseg"]

    # ---- segments ----------------------------------------------------
    def seg_of(self, linear):
        for s in self.segs:
            if s[0] <= linear < s[1]:
                return s
        return None

    @staticmethod
    def base(seg):
        """Linear address of offset 0 of a segment (paragraph * 16).  IDA
        segments may start unaligned (seg001 starts at 0x3BE8 but has
        paragraph 0x3BE), so offsets are relative to the paragraph."""
        return seg[2] * 16

    def fmt(self, linear):
        """Format a linear address as segname:offset (IDA style)."""
        s = self.seg_of(linear)
        if not s:
            return "0x%X" % linear
        return "%s:%04X" % (s[3], linear - self.base(s))

    def dos_seg(self, segname):
        return self.seg_by_name[segname][2] + DOS_SEG_DELTA

    def nearest_name(self, linear, max_delta=0x400):
        i = bisect.bisect_right(self.sorted_names, linear) - 1
        if i < 0:
            return None, None
        base = self.sorted_names[i]
        if linear - base > max_delta:
            return None, None
        return self.names[base], linear - base

    def describe(self, linear):
        n, d = self.nearest_name(linear)
        s = self.fmt(linear)
        if n is None:
            return s
        return "%s (%s%s)" % (s, n, "+%X" % d if d else "")

    # ---- address parsing --------------------------------------------
    def parse(self, text):
        """Parse any supported address form into an IDA linear address."""
        text = text.strip()
        if text in self.name_to_linear:
            return self.name_to_linear[text]
        m = re.match(r"^(\w+?):([0-9A-Fa-f]+)$", text)
        if m and m.group(1) in self.seg_by_name:
            return self.base(self.seg_by_name[m.group(1)]) + int(m.group(2), 16)
        if m:
            # DOS seg:off -> IDA seg = seg - 0x1A2
            seg = int(m.group(1), 16) - DOS_SEG_DELTA
            return seg * 16 + int(m.group(2), 16)
        return int(text, 16)

    # ---- overlays ----------------------------------------------------
    def stub_header(self, n):
        s = self.stubs[n]
        off = self.hdr + s[0]
        magic, _memswap, fileofs, codesize, relsize, nentries, _prev = struct.unpack_from("<HHIHHHH", self.data, off)
        assert magic == 0x3FCD, "stub%d has no int 3F header" % n
        return dict(stubfile=off, fileofs=self.ovr_base + fileofs, codesize=codesize,
                    relsize=relsize, nentries=nentries, dosseg=s[2] + DOS_SEG_DELTA)

    def stub_entries(self, n):
        h = self.stub_header(n)
        out = []
        for i in range(h["nentries"]):
            e = h["stubfile"] + 0x20 + 5 * i
            magic, off, _ = struct.unpack_from("<HHB", self.data, e)
            if magic != 0x3FCD:
                break
            out.append((0x20 + 5 * i, off))
        return out

    def segment_by_index(self, idx):
        """Borland overlay code stores far-call segment operands as
        8 * (index of the segment in the program's segment table); the overlay
        loader patches in the real segment.  Index N is IDA's segNNN for the
        root image and stubNNN for overlay stubs."""
        for cand in ("seg%03d" % idx, "stub%d" % idx):
            if cand in self.seg_by_name:
                return self.seg_by_name[cand]
        return None

    def far_target(self, from_seg, segv, offv):
        """Resolve a far call/jmp operand to a linear address."""
        if from_seg[3].startswith("ovr"):
            if segv % 8:
                return None
            tgt = self.segment_by_index(segv // 8)
            if tgt is None:
                return None
            return self.base(tgt) + offv
        return segv * 16 + offv

    def stub_target(self, n, stuboff):
        """Resolve stubNNN:stuboff to a linear address inside ovrNNN."""
        for so, o in self.stub_entries(n):
            if so == stuboff:
                return self.base(self.seg_by_name["ovr%d" % n]) + o
        return None

    def file_offset(self, linear):
        """File offset of the bytes at an IDA linear address (main image or overlay)."""
        s = self.seg_of(linear)
        assert s, "address %X is not in any segment" % linear
        m = re.match(r"ovr(\d+)$", s[3])
        if m:
            h = self.stub_header(int(m.group(1)))
            return h["fileofs"] + (linear - self.base(s))
        return self.hdr + linear

    def read(self, linear, length):
        f = self.file_offset(linear)
        return self.data[f:f + length]

    # ---- disassembly -------------------------------------------------
    def symbolize(self, line, seg):
        """Annotate an ndisasm line with symbols from the IDC."""
        notes = []

        def far(m):
            segv = int(m.group(2), 16)
            offv = int(m.group(3), 16)
            lin = self.far_target(seg, segv, offv)
            if lin is None:
                return "%s %s:%s ; (unresolved far target)" % (m.group(1), m.group(2), m.group(3))
            s = self.seg_of(lin)
            if s and s[3].startswith("stub"):
                n = int(s[3][4:])
                tgt = self.stub_target(n, offv)
                if tgt is not None:
                    return "%s %s:%04X -> %s" % (m.group(1), s[3], offv, self.describe(tgt))
            return "%s %s" % (m.group(1), self.describe(lin))
        line = re.sub(r"(call|jmp)\s+(0x[0-9a-f]+):(0x[0-9a-f]+)", far, line)

        def near(m):
            lin = self.base(seg) + int(m.group(2), 16)
            return "%s %s" % (m.group(1), self.describe(lin))
        line = re.sub(r"\b(call|jmp|jmp short|j[a-z]+)\s+(0x[0-9a-f]+)\b", near, line)

        for m in re.finditer(r"\[(?:(bx|si|di|bx\+si|bx\+di)?([+-]))?(0x[0-9a-f]+)\]", line):
            reg, sign, disp = m.group(1), m.group(2), m.group(3)
            v = int(disp, 16)
            if sign == "-":
                v = (0x10000 - v) & 0xFFFF
            if reg is None and sign is None:
                pass  # absolute [0xNNNN]
            elif reg is None:
                continue  # [bp+..] style handled below
            if v < 0x100 and reg is not None:
                continue  # small struct field offsets
            lin = self.base(self.dseg) + v
            n, d = self.nearest_name(lin, 0x1000)
            if n:
                notes.append("dseg:%04X=%s%s" % (v, n, "+%X" % d if d else ""))
        if notes:
            line = "%-58s ; %s" % (line, ", ".join(notes))
        return line

    def disasm(self, linear, length):
        seg = self.seg_of(linear)
        assert seg, "address %X is not in any segment" % linear
        code = self.read(linear, length)
        p = subprocess.run(["ndisasm", "-b", "16", "-o", "0x%x" % (linear - self.base(seg)), "-"],
                           input=code, capture_output=True)
        out = []
        for line in p.stdout.decode().splitlines():
            m = re.match(r"^([0-9A-F]+)\s+([0-9A-F]+)\s+(.*)$", line)
            if not m:
                out.append(line)
                continue
            off = int(m.group(1), 16)
            lin = self.base(seg) + off
            if lin in self.names:
                out.append("")
                out.append("%s:%04X  %s:   ; %s" % (seg[3], off, self.names[lin], self.fmt(lin)))
            for c in self.comments.get(lin, []):
                out.append("        ; %s" % c)
            out.append("%s:%04X  %-16s %s" % (seg[3], off, m.group(2), self.symbolize(m.group(3), seg)))
        return "\n".join(out)

    def function_end(self, linear):
        i = bisect.bisect_right(self.sorted_names, linear)
        seg = self.seg_of(linear)
        assert seg, "address %X is not in any segment" % linear
        while i < len(self.sorted_names):
            nxt = self.sorted_names[i]
            if nxt >= seg[1]:
                break
            if not re.match(r"(loc|off|word|byte|dword|unk|asc)_", self.names[nxt]):
                return nxt
            i += 1
        return seg[1]

    def callers(self, target_seg, off):
        """Find far calls/jumps to a segment:offset anywhere in the image.
        The root image encodes the real paragraph; overlay code encodes
        8 * segment-index (see far_target)."""
        idx = int(re.sub(r"\D", "", target_seg[3]))
        out = []
        for opcode in (0x9A, 0xEA):
            root_pat = bytes([opcode, off & 0xFF, off >> 8, target_seg[2] & 0xFF, target_seg[2] >> 8])
            ovr_pat = bytes([opcode, off & 0xFF, off >> 8, (idx * 8) & 0xFF, (idx * 8) >> 8])
            i = self.data.find(root_pat)
            while i >= 0:
                if i < self.fbov:
                    out.append("%s  (root, far %s)" % (self.describe(self.file_to_linear(i)), "call" if opcode == 0x9A else "jmp"))
                i = self.data.find(root_pat, i + 1)
            i = self.data.find(ovr_pat)
            while i >= 0:
                lin = self.file_to_linear(i)
                if i >= self.fbov and lin is not None:
                    out.append("%s  (overlay, far %s)" % (self.describe(lin), "call" if opcode == 0x9A else "jmp"))
                i = self.data.find(ovr_pat, i + 1)
        return out

    def file_to_linear(self, f):
        if f < self.fbov:
            return f - self.hdr
        for n in self.stubs:
            h = self.stub_header(n)
            if h["fileofs"] <= f < h["fileofs"] + h["codesize"]:
                return self.base(self.seg_by_name["ovr%d" % n]) + (f - h["fileofs"])
        return None


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        print(__doc__)
        return 0
    img = Image()
    cmd = argv[1]
    if cmd == "sym":
        lin = img.parse(argv[2])
        print("linear   0x%X" % lin)
        print("ida      %s" % img.describe(lin))
        s = img.seg_of(lin)
        if s and s[3].startswith("stub"):
            tgt = img.stub_target(int(s[3][4:]), lin - img.base(s))
            print("dos      %04X:%04X" % (s[2] + DOS_SEG_DELTA, lin - img.base(s)))
            if tgt is not None:
                print("target   %s" % img.describe(tgt))
        elif s and not s[3].startswith("ovr"):
            print("dos      %04X:%04X" % (s[2] + DOS_SEG_DELTA, lin - img.base(s)))
        # reverse: which stub entries point at this overlay address?
        if s and s[3].startswith("ovr"):
            n = int(s[3][3:])
            for so, o in img.stub_entries(n):
                if o == lin - img.base(s):
                    print("stub     stub%d:%04X  (DOS %04X:%04X)" % (n, so, img.stubs[n][2] + DOS_SEG_DELTA, so))
        for c in img.comments.get(lin, []):
            print("comment  %s" % c)
    elif cmd == "dis":
        lin = img.parse(argv[2])
        s = img.seg_of(lin)
        if s and s[3].startswith("stub"):
            lin = img.stub_target(int(s[3][4:]), lin - img.base(s))
        assert lin is not None, "no overlay function behind that stub entry"
        length = int(argv[3], 16) if len(argv) > 3 else img.function_end(lin) - lin
        print("; %s  length 0x%X" % (img.describe(lin), length))
        print(img.disasm(lin, length))
    elif cmd == "callers":
        lin = img.parse(argv[2])
        s = img.seg_of(lin)
        assert s and not s[3].startswith("ovr"), "callers wants a root or stubNNN:off address (overlay functions are reached through their stub)"
        for l in img.callers(s, lin - img.base(s)):
            print(l)
    elif cmd == "segments":
        for sg in img.segs:
            print("%-8s para %04X  linear %05X..%05X  dos %04X" % (sg[3], sg[2], sg[0], sg[1], sg[2] + DOS_SEG_DELTA))
    elif cmd == "entries":
        n = int(re.sub(r"\D", "", argv[2]))
        h = img.stub_header(n)
        print("stub%d DOS seg %04X, overlay code at file 0x%X size 0x%X" % (n, h["dosseg"], h["fileofs"], h["codesize"]))
        for so, o in img.stub_entries(n):
            print("stub%d:%04X -> %s" % (n, so, img.describe(img.base(img.seg_by_name["ovr%d" % n]) + o)))
    elif cmd == "names":
        want = argv[2] if len(argv) > 2 else None
        for lin in img.sorted_names:
            f = img.fmt(lin)
            if want is None or f.startswith(want + ":"):
                print("%-16s %s" % (f, img.names[lin]))
    elif cmd == "selfcheck":
        ok = bad = 0
        for lin, name in img.names.items():
            s = img.seg_of(lin)
            if not s or not s[3].startswith("ovr") or re.match(r"(loc|off|word|byte|dword|unk|asc|a[A-Z])", name):
                continue
            if img.read(lin, 3) == b"\x55\x8b\xec":
                ok += 1
            else:
                bad += 1
                print("no prologue at %s %s: %s" % (img.fmt(lin), name, img.read(lin, 6).hex()))
        print("%d named overlay functions start with push bp/mov bp,sp; %d do not" % (ok, bad))
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
