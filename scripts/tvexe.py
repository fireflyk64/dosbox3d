#!/usr/bin/env python3
"""Terminal Velocity's GAME.EXE: the LE program inside the DOS/4G bound executable.

    scripts/tvexe.py GAME.EXE info            the header and the objects
    scripts/tvexe.py GAME.EXE image OUT       the objects laid out at their image addresses, fixups applied
                                              (OUT.bin from image base 0, OUT.map: one line per object)
    scripts/tvexe.py GAME.EXE dis ADDR [N]    N instructions from image address ADDR (hex)
    scripts/tvexe.py GAME.EXE str TEXT        image addresses of TEXT, and the code that references them
    scripts/tvexe.py GAME.EXE xref ADDR       the instructions that hold the 32-bit value ADDR (hex)

Image addresses are the LE's own (object base + offset); docs/terminalvelocity.md says where DOS/4GW puts them.
"""
import struct, sys

def find_le(d):
    i = d.find(b'LE\x00\x00')
    while i >= 0:
        # a plausible header: 386 cpu, some objects
        cpu, nobj = struct.unpack_from('<H', d, i + 8)[0], struct.unpack_from('<I', d, i + 0x44)[0]
        if cpu in (2, 3) and 0 < nobj < 64:
            return i
        i = d.find(b'LE\x00\x00', i + 1)
    raise SystemExit('no LE header')

class LE:
    def __init__(self, path):
        self.d = d = open(path, 'rb').read()
        self.h = h = find_le(d)
        u = lambda o: struct.unpack_from('<I', d, h + o)[0]
        self.npages = u(0x14); self.eip_obj = u(0x18); self.eip = u(0x1C)
        self.esp_obj = u(0x20); self.esp = u(0x24); self.pagesize = u(0x28); self.lastpage = u(0x2C)
        self.objtab = h + u(0x40); self.nobj = u(0x44); self.pagetab = h + u(0x48)
        self.fixpage = h + u(0x68); self.fixrec = h + u(0x6C)
        self.datapages = u(0x80)   # from the start of the file of the LE... (bound: relative to the MZ stub start)
        self.stub = 0
        i = d.rfind(b'MZ', 0, h)
        while i >= 0:
            if i + 0x40 <= len(d) and i + struct.unpack_from('<I', d, i + 0x3C)[0] == h:
                self.stub = i; break
            i = d.rfind(b'MZ', 0, i)
        self.objs = []
        for k in range(self.nobj):
            vsize, base, flags, pidx, pcount, _ = struct.unpack_from('<IIIIII', d, self.objtab + 24 * k)
            self.objs.append(dict(vsize=vsize, base=base, flags=flags, pidx=pidx, pcount=pcount))

    def page_file_offset(self, p):   # p 1-based
        # LE page table: 4 bytes per page, high 3 bytes page number (big endian-ish), low byte flags
        e = self.d[self.pagetab + 4 * (p - 1): self.pagetab + 4 * p]
        num = (e[0] << 16) | (e[1] << 8) | e[2]
        return self.datapages_abs + (num - 1) * self.pagesize

    def load(self):
        # data pages offset: from the start of the program the LE belongs to (in a DOS/4G bound
        # executable, the MZ stub whose e_lfanew names this header, past the extender)
        self.datapages_abs = self.datapages + self.stub
        if self.datapages > len(self.d):
            raise SystemExit('data pages offset past the file')
        top = max(o['base'] + o['vsize'] for o in self.objs)
        img = bytearray(top + 0x1000)
        for k, o in enumerate(self.objs):
            for j in range(o['pcount']):
                p = o['pidx'] + j
                off = self.page_file_offset(p)
                size = self.pagesize if p != self.npages else self.lastpage
                dst = o['base'] + j * self.pagesize
                img[dst:dst + size] = self.d[off:off + size]
        self.apply_fixups(img)
        self.img = img
        return img

    def apply_fixups(self, img):
        d = self.d
        self.fixcount = 0
        self.fixed = set()          # image addresses of bytes a fixup wrote
        for k, o in enumerate(self.objs):
            for j in range(o['pcount']):
                p = o['pidx'] + j
                a = struct.unpack_from('<I', d, self.fixpage + 4 * (p - 1))[0]
                b = struct.unpack_from('<I', d, self.fixpage + 4 * p)[0]
                i = self.fixrec + a; end = self.fixrec + b
                pagebase = o['base'] + j * self.pagesize
                while i < end:
                    src, flags = d[i], d[i + 1]; i += 2
                    if src & 0x20:
                        cnt = d[i]; i += 1; offs = None
                    else:
                        offs = [struct.unpack_from('<h', d, i)[0]]; i += 2
                    if flags & 0x03 != 0:
                        raise SystemExit('fixup kind %x not handled' % flags)
                    if flags & 0x40:
                        tobj = struct.unpack_from('<H', d, i)[0]; i += 2
                    else:
                        tobj = d[i]; i += 1
                    stype = src & 0x0F
                    if stype == 2:      # 16-bit selector: nothing to place
                        toff = 0
                    elif flags & 0x10:
                        toff = struct.unpack_from('<I', d, i)[0]; i += 4
                    else:
                        toff = struct.unpack_from('<H', d, i)[0]; i += 2
                    if src & 0x20:
                        offs = [struct.unpack_from('<h', d, i + 2 * q)[0] for q in range(cnt)]; i += 2 * cnt
                    target = self.objs[tobj - 1]['base'] + toff
                    for so in offs:
                        at = pagebase + so
                        if stype in (7, 8):
                            self.fixed.update(range(at, at + 4))
                        elif stype == 5:
                            self.fixed.update(range(at, at + 2))
                        if stype == 7:      # 32-bit offset
                            if 0 <= at and at + 4 <= len(img):
                                struct.pack_into('<I', img, at, target)
                        elif stype == 8:    # 32-bit self-relative
                            if 0 <= at and at + 4 <= len(img):
                                struct.pack_into('<I', img, at, (target - (at + 4)) & 0xFFFFFFFF)
                        elif stype == 5:    # 16-bit offset
                            if 0 <= at and at + 2 <= len(img):
                                struct.pack_into('<H', img, at, target & 0xFFFF)
                        self.fixcount += 1

def code_range(le):
    o = le.objs[0]
    return o['base'], o['base'] + o['vsize']

_targets = None
def call_targets(le, img):
    global _targets
    if _targets is None:
        lo, hi = code_range(le)
        t = {}
        for i in range(lo, hi - 5):
            if img[i] == 0xE8:
                d = struct.unpack_from('<i', img, i + 1)[0]
                a = i + 5 + d
                if lo <= a < hi:
                    t[a] = t.get(a, 0) + 1
        # keep targets called 2+ times or starting with a usual prologue
        _targets = {a: n for a, n in t.items() if n >= 2 or img[a] in (0x53, 0x51, 0x52, 0x56, 0x57, 0x55, 0x83, 0x81, 0x89, 0x8b)}
    return _targets

def callers(le, img, target):
    lo, hi = code_range(le)
    out = []
    for i in range(lo, hi - 5):
        if img[i] == 0xE8 and i + 5 + struct.unpack_from('<i', img, i + 1)[0] == target:
            out.append(i)
    return out

def func_of(le, img, addr):
    starts = sorted(call_targets(le, img))
    import bisect
    k = bisect.bisect_right(starts, addr) - 1
    return starts[k] if k >= 0 else 0

def main():
    le = LE(sys.argv[1]); cmd = sys.argv[2] if len(sys.argv) > 2 else 'info'
    if cmd == 'info':
        print('LE header at file 0x%x (program at 0x%x), %d pages of %d, last %d, data pages at 0x%x' % (le.h, le.stub, le.npages, le.pagesize, le.lastpage, le.stub + le.datapages))
        print('entry object %d + 0x%x, stack object %d + 0x%x' % (le.eip_obj, le.eip, le.esp_obj, le.esp))
        for k, o in enumerate(le.objs):
            print('object %d: base 0x%x size 0x%x flags 0x%x pages %d..%d' % (k + 1, o['base'], o['vsize'], o['flags'], o['pidx'], o['pidx'] + o['pcount'] - 1))
        return
    img = le.load()
    if cmd == 'image':
        open(sys.argv[3] + '.bin', 'wb').write(img)
        with open(sys.argv[3] + '.map', 'w') as f:
            for k, o in enumerate(le.objs):
                f.write('%d 0x%x 0x%x 0x%x\n' % (k + 1, o['base'], o['vsize'], o['flags']))
        print('%d fixups applied' % le.fixcount)
    elif cmd == 'dis':
        import capstone
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        a = int(sys.argv[3], 16); n = int(sys.argv[4]) if len(sys.argv) > 4 else 40
        for ins in md.disasm(bytes(img[a:a + 16 * n]), a):
            print('%08x  %-24s %s %s' % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str)); n -= 1
            if n <= 0: break
    elif cmd == 'str':
        t = sys.argv[3].encode()
        i = img.find(t)
        while i >= 0:
            print('0x%x: %r' % (i, bytes(img[i:i + 60]).split(b'\0')[0]))
            j = img.find(struct.pack('<I', i))
            while j >= 0:
                print('   referenced at 0x%x' % j); j = img.find(struct.pack('<I', i), j + 1)
            i = img.find(t, i + 1)
    elif cmd == 'funcs':
        # every direct call's target in the code object, with how many call it
        for a, n in sorted(call_targets(le, img).items()):
            print('%08x %d' % (a, n))
    elif cmd == 'callers':
        t = int(sys.argv[3], 16)
        for at in callers(le, img, t):
            print('%08x  (in %08x)' % (at, func_of(le, img, at)))
    elif cmd == 'func':
        print('%08x' % func_of(le, img, int(sys.argv[3], 16)))
    elif cmd == 'xref':
        v = struct.pack('<I', int(sys.argv[3], 16)); j = img.find(v)
        while j >= 0:
            print('0x%x' % j); j = img.find(v, j + 1)

if __name__ == '__main__':
    main()
