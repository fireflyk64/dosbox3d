#!/usr/bin/env python3
"""The TIE Fighter demo's hot renderer routines, translated to C++ one instruction at a time
(src/cpu/wcnet_tie640_rc.inc, used by wcnet_tie640_fast.cpp).

  scripts/tie640-recompile.py <TIEMISS.EXE> > src/cpu/wcnet_tie640_rc.inc

The routines (image paragraph 1CCA, the renderer) are followed from their entries through every jump, near call and
far call into the same segment; each instruction becomes the statements DOSBox's own normal core runs for it (its
macros of instructions.h, its lazy flags, its registers and memory calls), so the translation leaves the CPU and the
memory as the interpreter would, instruction for instruction.  Control that leaves the set (a far call or return
elsewhere, anything not translated) stores CS:IP and returns to the interpreter, which carries on from there; the
return addresses of the far calls that leave are entries too (the hook's places).

The file holds no byte of the game: only the translation.  A checksum of the instructions' bytes (relocated words
left out) is checked against memory before the translation is used.
"""
import sys, struct, collections
import capstone
from capstone.x86 import *

# The segments translated (image paragraphs): the flight's scene section, draw.c, anim.c's drawing, fview.c, the
# resource lookup, the renderer.  A far call into one of them is followed; one elsewhere (the overlay, the stars)
# leaves the translation, and the place it comes back to is an entry.
TRANSLATE = (0x0090, 0x0464, 0x168C, 0x1768, 0x18AA, 0x1CCA)
# Not followed though in those segments: the sprite scaler (rotscale_rotatescaleimage: it runs code it writes).
EXTERNAL = {(0x1CCA, 0xA02A)}
# The entries: tie_updatescreen's scene section (0090:0B1A, run to its end 0090:1069 and back by tie640's hook: the
# four quarters in one run); and routines the rest of the frame calls (the panel's own small 3D picture):
# xtrans2_drawxtrans, logbuf2_clearbuffer, xtrans2_clearruntable, xtrans2_initxtrans, rotscale_preparefastdraw, the
# sprite scaler's scan of its picture into the scanline pass's lists (near, in rotscale_rotatescaleimage),
# drawpol_drawpolyobject.
ENTRIES = [(0x0090, 0x0B1A), (0x1CCA, 0x85E9), (0x1CCA, 0xD772), (0x1CCA, 0x8510), (0x1CCA, 0x8571), (0x1CCA, 0x9FA2),
           (0x1CCA, 0xA132), (0x1CCA, 0x0AAB)]
NEAR_ENTRIES = [(0x1CCA, 0xA132)]
# jump tables in the code (words, offsets in their segment): (segment, offset): count
TABLES = {(0x1CCA, 0x8731): 18, (0x1CCA, 0x8823): 10, (0x1CCA, 0x8B74): 10, (0x1CCA, 0x8D51): 18, (0x0090, 0x0DD2): 15,
          (0x168C, 0x0850): 8, (0x168C, 0x0906): 8}
# near calls through tables of the data segment (image paragraph 421F): (code segment, offset, words)
DS_TABLES = [(0x1CCA, 0x26E5, 16), (0x1CCA, 0x2705, 16)]
DGROUP = 0x421F
# The places hooked (wcnet_tie640.cpp's tie640_watch and wcnet_tie640_fast.cpp's): the translation calls the hook
# itself where it passes one (any segment's instruction at such an offset: the hook looks at CS).
HOOK_IPS = [0x0B1A, 0x0B36, 0x1069, 0x0F0E, 0x0775, 0xDF1D, 0xDF26, 0x09FC, 0x0C5B, 0x1226, 0x139C, 0x112A, 0x1310, 0x14F6, 0x0782, 0x0D0A,
            0x0D4A, 0x0FA1, 0xA02A]
# tie640's own changes of the code while the quarters are drawn (wcnet_tie640.cpp, kScalePatches): the translation
# runs whichever is in memory
PSEG = 0x1CCA
PATCHES = {
    0x7820: (bytes([0x08]), bytes([0x09])), 0x7824: (bytes([0x08]), bytes([0x09])),
    0x78BE: (bytes([0x08]), bytes([0x09])), 0x78C2: (bytes([0x08]), bytes([0x09])),
}
for _p in (0x7A27, 0x7AF7, 0x7C11, 0x7CA1):
    PATCHES[_p] = (bytes([0x8A, 0xF2, 0x8A, 0xD4, 0x8A, 0xE0, 0x32, 0xC0]), bytes([0x0F, 0xA4, 0xC2, 0x09, 0xC1, 0xE0, 0x09, 0x90]))
# A place control comes back to is watched by its IP alone, and the same IP in another segment's busy loop costs a
# trip out of the CPU's loop each time: so the place is moved a few instructions on where that was seen (the
# profile's "hooks reached by place"), or not watched at all where the call is rare (the rest of that quarter is
# then the interpreter's until the next entry).
RETURN_AT = {(0x168C, 0x078E): 0x0793, (0x1768, 0x0586): 0x058A}
RETURN_DROP = {(0x0090, 0x0B5C), (0x0090, 0x1011), (0x0090, 0x101C)}
CHUNK = 900     # instructions a C++ function (one giant function takes the compiler minutes)

exe = open(sys.argv[1], 'rb').read()
hdr = struct.unpack_from('<H', exe, 8)[0] * 16
img = exe[hdr:]
nrel, = struct.unpack_from('<H', exe, 6)
relo, = struct.unpack_from('<H', exe, 24)
relocs = set()
for i in range(nrel):
    o, s = struct.unpack_from('<HH', exe, relo + 4 * i)
    relocs.add(s * 16 + o)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
md.detail = True

# --- the instructions reached (addresses are (segment, offset)) ----------------------------------------------------
seen = {}
targets = set(ENTRIES)       # every address control can arrive at other than by falling through
work = list(ENTRIES)
returns = set()              # where control comes back into the translation: after a far call that leaves, after an
                             # instruction left to the interpreter
for (s, a), n in TABLES.items():
    for i in range(n):
        t = (s, struct.unpack_from('<H', img, s * 16 + a + 2 * i)[0])
        targets.add(t)
        work.append(t)
for s, a, n in DS_TABLES:
    for i in range(n):
        t = (s, struct.unpack_from('<H', img, DGROUP * 16 + a + 2 * i)[0])
        targets.add(t)
        work.append(t)
for a, (was, now) in PATCHES.items():
    if img[PSEG * 16 + a:PSEG * 16 + a + len(was)] != was:
        sys.exit('patch place %04X is not as known' % a)


def translated(s, o):
    return s in TRANSLATE and (s, o) not in EXTERNAL


while work:
    seg, a = work.pop()
    while (seg, a) not in seen:
        ins = next(md.disasm(img[seg * 16 + a:seg * 16 + a + 16], a), None)
        if ins is None:
            sys.exit('undecodable at %04X:%04X' % (seg, a))
        seen[(seg, a)] = ins
        m, ops, nxt = ins.mnemonic, ins.operands, a + ins.size
        if m.startswith('j') or m.startswith('loop'):
            if ops and ops[0].type == X86_OP_IMM:
                t = (seg, ops[0].imm & 0xFFFF)
                targets.add(t)
                work.append(t)
            if ops and ops[0].type == X86_OP_MEM and ops[0].mem.disp and (seg, ops[0].mem.disp & 0xFFFF) not in TABLES:
                sys.exit('a jump through a table not given in TABLES: %04X:%04X %s' % (seg, a, ins.op_str))
            if m == 'jmp':
                break
        elif m == 'call':
            if ops[0].type == X86_OP_IMM:
                t = (seg, ops[0].imm & 0xFFFF)
                targets.add(t)
                work.append(t)
            targets.add((seg, nxt))
        elif m == 'lcall':
            s, o = [int(x, 16) for x in ins.op_str.split(', ')]
            if translated(s, o):
                targets.add((s, o))
                work.append((s, o))
            else:
                returns.add((seg, nxt))
            targets.add((seg, nxt))
        elif m == 'int':
            targets.add((seg, nxt))     # (run here when its handler is the emulator's own: rc_callback)
        elif m in ('into', 'in', 'out', 'cli', 'sti', 'pushf', 'popf', 'pushfd', 'popfd', 'lahf', 'sahf'):
            returns.add((seg, nxt))
            targets.add((seg, nxt))
        elif m in ('ret', 'retf', 'iret', 'int3', 'hlt'):
            break
        a = nxt
addrs = sorted(seen)
cur_seg = 0

R8 = {X86_REG_AL: 'reg_al', X86_REG_AH: 'reg_ah', X86_REG_BL: 'reg_bl', X86_REG_BH: 'reg_bh', X86_REG_CL: 'reg_cl',
      X86_REG_CH: 'reg_ch', X86_REG_DL: 'reg_dl', X86_REG_DH: 'reg_dh'}
R16 = {X86_REG_AX: 'reg_ax', X86_REG_BX: 'reg_bx', X86_REG_CX: 'reg_cx', X86_REG_DX: 'reg_dx', X86_REG_SI: 'reg_si',
       X86_REG_DI: 'reg_di', X86_REG_BP: 'reg_bp', X86_REG_SP: 'reg_sp'}
R32 = {X86_REG_EAX: 'reg_eax', X86_REG_EBX: 'reg_ebx', X86_REG_ECX: 'reg_ecx', X86_REG_EDX: 'reg_edx', X86_REG_ESI: 'reg_esi',
       X86_REG_EDI: 'reg_edi', X86_REG_EBP: 'reg_ebp', X86_REG_ESP: 'reg_esp'}
SEGS = {X86_REG_ES: 'es', X86_REG_CS: 'cs', X86_REG_SS: 'ss', X86_REG_DS: 'ds', X86_REG_FS: 'fs', X86_REG_GS: 'gs'}
TY = {1: 'Bit8u', 2: 'Bit16u', 4: 'Bit32u'}
STY = {1: 'Bit8s', 2: 'Bit16s', 4: 'Bit32s'}
SUF = {1: 'B', 2: 'W', 4: 'D'}
LF = {1: 'b', 2: 'w', 4: 'd'}
RD = {1: 'mem_readb_inline', 2: 'mem_readw_inline', 4: 'mem_readd_inline'}
WR = {1: 'mem_writeb_inline', 2: 'mem_writew_inline', 4: 'mem_writed_inline'}


class Unsupported(Exception):
    pass


def has_reloc(ins):
    base = cur_seg * 16
    return [r - base - ins.address for r in range(base + ins.address, base + ins.address + ins.size) if r in relocs]


def reg(r):
    for t in (R8, R16, R32):
        if r in t:
            return t[r]
    raise Unsupported('register %d' % r)


def ea(ins, op):
    """the linear address of a memory operand"""
    m = op.mem
    wide = ins.addr_size == 4
    parts = []
    if m.base:
        parts.append(reg(m.base))
    if m.index:
        parts.append('%s * %d' % (reg(m.index), m.scale) if m.scale != 1 else reg(m.index))
    if m.disp or not parts:
        parts.append('%d' % m.disp)
    off = '(%s)(%s)' % ('Bit32u' if wide else 'Bit16u', ' + '.join(parts))
    if m.segment:
        seg = SEGS[m.segment]
    else:
        seg = 'ss' if m.base in (X86_REG_BP, X86_REG_EBP, X86_REG_ESP, X86_REG_SP) else 'ds'
    return 'SegPhys(%s) + %s' % (seg, off)


def imm(ins, op):
    v = op.imm & ((1 << (8 * op.size)) - 1)
    if has_reloc(ins):
        return '(Bit16u)(0x%X + load)' % v        # (a segment: the image's paragraph, relocated)
    return '0x%X' % v


def rd(ins, op):
    """an expression for an operand's value"""
    if op.type == X86_OP_REG:
        if op.reg in SEGS:
            return 'SegValue(%s)' % SEGS[op.reg]
        return reg(op.reg)
    if op.type == X86_OP_IMM:
        return imm(ins, op)
    return '%s(%s)' % (RD[op.size], ea(ins, op))


def rm(ins, op, macro_args):
    """the (op1, load, save) of DOSBox's macros for a destination operand; a prelude statement for memory"""
    if op.type == X86_OP_REG:
        return '', reg(op.reg), 'RC_LR', 'RC_SR'
    return 'const PhysPt a_ = %s; ' % ea(ins, op), 'a_', RD[op.size], WR[op.size]


def wr(ins, op, value):
    if op.type == X86_OP_REG:
        if op.reg in SEGS:
            return 'SegSet16(%s, %s);' % (SEGS[op.reg], value)
        return '%s = %s;' % (reg(op.reg), value)
    return '%s(%s, %s);' % (WR[op.size], ea(ins, op), value)


CC = {'jo': 'TFLG_O', 'jno': 'TFLG_NO', 'jb': 'TFLG_B', 'jae': 'TFLG_NB', 'je': 'TFLG_Z', 'jne': 'TFLG_NZ', 'jbe': 'TFLG_BE',
      'ja': 'TFLG_NBE', 'js': 'TFLG_S', 'jns': 'TFLG_NS', 'jl': 'TFLG_L', 'jge': 'TFLG_NL', 'jle': 'TFLG_LE', 'jg': 'TFLG_NLE'}


def fused(kind, size, cc):
    """a jump's condition straight from the lazy flags' operands, when the instruction that set them is known"""
    a, b, r = 'lf_var1' + LF[size], 'lf_var2' + LF[size], 'lf_res' + LF[size]
    sa, sb, sr = '(%s)%s' % (STY[size], a), '(%s)%s' % (STY[size], b), '(%s)%s' % (STY[size], r)
    if kind == 'sub':
        t = {'je': '%s == %s' % (a, b), 'jne': '%s != %s' % (a, b), 'jb': '%s < %s' % (a, b), 'jae': '%s >= %s' % (a, b),
             'jbe': '%s <= %s' % (a, b), 'ja': '%s > %s' % (a, b), 'jl': '%s < %s' % (sa, sb), 'jge': '%s >= %s' % (sa, sb),
             'jle': '%s <= %s' % (sa, sb), 'jg': '%s > %s' % (sa, sb), 'js': '%s < 0' % sr, 'jns': '%s >= 0' % sr}
    elif kind == 'logic':
        t = {'je': '%s == 0' % r, 'jne': '%s != 0' % r, 'jb': 'false', 'jae': 'true', 'jbe': '%s == 0' % r, 'ja': '%s != 0' % r,
             'jl': '%s < 0' % sr, 'jge': '%s >= 0' % sr, 'jle': '%s <= 0' % sr, 'jg': '%s > 0' % sr, 'js': '%s < 0' % sr,
             'jns': '%s >= 0' % sr}
    else:       # the result only
        t = {'je': '%s == 0' % r, 'jne': '%s != 0' % r, 'js': '%s < 0' % sr, 'jns': '%s >= 0' % sr}
    return t.get(cc)


NEUTRAL = {'lodsb', 'lodsw', 'mov', 'movzx', 'movsx', 'lea', 'push', 'pop', 'xchg', 'not', 'les', 'lds', 'stosb', 'stosw', 'movsb', 'movsw',
           'rep stosb', 'rep stosw', 'rep movsw', 'rep movsb', 'cwd', 'cbw', 'cdq', 'cwde', 'jmp', 'loop'} | set(CC)
flag_src = None     # (kind, size) of the instruction whose lazy flags stand, while known

unsupported = collections.Counter()
chunk_of = {}       # (segment, offset) -> the C++ function it is in
cur_chunk = 0


def goto(t):
    """control to a translated place of this segment"""
    t = (cur_seg, t)
    if chunk_of.get(t) == cur_chunk:
        return 'goto L_%04X;' % t[1]
    if t in chunk_of:
        return '{ ip_ = 0x%04X; return %d; }' % (t[1], chunk_of[t])
    return '{ ip_ = 0x%04X; return -1; }' % t[1]


def translate(ins):
    global flag_src
    a, m, ops, nxt = ins.address, ins.mnemonic, ins.operands, ins.address + ins.size
    b = ins.bytes
    PREFIXES = (0x66, 0x67, 0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0xF2, 0xF3)
    npre = 0
    while b[npre] in PREFIXES:
        npre += 1
    pre66 = 0x66 in b[:npre]
    opc = b[npre]
    if opc == 0x98:
        m = 'cwde' if pre66 else 'cbw'
    if opc == 0x99:
        m = 'cdq' if pre66 else 'cwd'
    if (cur_seg, a) in targets:
        flag_src = None
    src = flag_src
    if m not in NEUTRAL:
        flag_src = None
    # --- control
    if m in CC:
        t = ops[0].imm & 0xFFFF
        cond = fused(src[0], src[1], m) if src else None
        return 'if (%s) %s' % (cond or CC[m], goto(t))
    if m == 'jmp':
        if ops[0].type == X86_OP_IMM:
            return goto(ops[0].imm & 0xFFFF)
        return '{ ip = %s; goto rc_dispatch; }' % rd(ins, ops[0])
    if m == 'loop':
        return 'if (--%s) %s' % ('reg_ecx' if ins.addr_size == 4 else 'reg_cx', goto(ops[0].imm & 0xFFFF))
    if m == 'call':
        if ops[0].type != X86_OP_IMM:
            return '{ const Bit16u t_ = %s; CPU_Push16(0x%04X); ip = t_; goto rc_dispatch; }' % (rd(ins, ops[0]), nxt)
        return '{ CPU_Push16(0x%04X); %s }' % (nxt, goto(ops[0].imm & 0xFFFF))
    if m == 'lcall':
        s, o = [int(x, 16) for x in ins.op_str.split(', ')]
        if s == cur_seg and translated(s, o):
            return '{ FillFlags(); CPU_Push16(SegValue(cs)); CPU_Push16(0x%04X); %s }' % (nxt, goto(o))
        where = chunk_of.get((s, o), -1) if translated(s, o) else -1
        return '{ FillFlags(); CPU_Push16(SegValue(cs)); CPU_Push16(0x%04X); SegSet16(cs, (Bit16u)(0x%04X + load)); ip_ = 0x%04X; return %d; }' % (nxt, s, o, where)
    if m == 'ret':
        extra = ' reg_sp += %d;' % ops[0].imm if ops else ''
        return '{ ip = CPU_Pop16();%s goto rc_dispatch; }' % extra
    if m == 'retf':
        extra = ' reg_sp += %d;' % ops[0].imm if ops else ''
        return '{ FillFlags(); ip = CPU_Pop16(); const Bit16u seg_ = CPU_Pop16();%s if (seg_ == SegValue(cs)) goto rc_dispatch; SegSet16(cs, seg_); ip_ = ip; return rc_find((Bit16u)(seg_ - load), ip); }' % extra
    if m == 'int':
        # the interrupt as the core takes it; a handler that is the emulator's own callback (EMS's int 67h) is called
        # and returned from here, anything else is the interpreter's from the handler's first instruction
        return '{ CPU_Interrupt(0x%02X, CPU_INT_SOFTWARE, 0x%04X); if (!rc_callback()) { ip_ = (Bit16u)reg_eip; return -1; } ip_ = ip = (Bit16u)reg_eip; if (SegValue(cs) != (Bit16u)(0x%04X + load)) return rc_find((Bit16u)(SegValue(cs) - load), ip); goto rc_dispatch; }' % (
            ops[0].imm & 0xFF, nxt, cur_seg)
    # --- moves
    if m == 'mov':
        return wr(ins, ops[0], rd(ins, ops[1]))
    if m == 'movzx':
        return wr(ins, ops[0], '(%s)%s' % (TY[ops[0].size], rd(ins, ops[1])))
    if m == 'movsx':
        return wr(ins, ops[0], '(%s)(%s)(%s)%s' % (TY[ops[0].size], STY[ops[0].size], STY[ops[1].size], rd(ins, ops[1])))
    if m == 'lea':
        mm = ops[1].mem
        parts = [reg(mm.base)] if mm.base else []
        if mm.index:
            parts.append('%s * %d' % (reg(mm.index), mm.scale))
        parts.append('%d' % mm.disp)
        return wr(ins, ops[0], '(%s)(%s)' % (TY[ops[0].size], ' + '.join(parts)))
    if m in ('les', 'lds'):
        return '{ const PhysPt a_ = %s; const Bit16u o_ = mem_readw_inline(a_); SegSet16(%s, mem_readw_inline(a_ + 2)); %s = o_; }' % (
            ea(ins, ops[1]), m[1:] , reg(ops[0].reg))
    if m == 'xchg':
        n = ops[0].size
        if ops[0].type == X86_OP_REG and ops[1].type == X86_OP_REG:
            return '{ const %s t_ = %s; %s = %s; %s = t_; }' % (TY[n], reg(ops[0].reg), reg(ops[0].reg), reg(ops[1].reg), reg(ops[1].reg))
        mo, ro = (ops[0], ops[1]) if ops[0].type == X86_OP_MEM else (ops[1], ops[0])
        return '{ const PhysPt a_ = %s; const %s t_ = %s(a_); %s(a_, %s); %s = t_; }' % (ea(ins, mo), TY[n], RD[n], WR[n], reg(ro.reg), reg(ro.reg))
    if m == 'push':
        if ops[0].size == 4:
            return 'CPU_Push32(%s);' % rd(ins, ops[0])
        return 'CPU_Push16(%s);' % rd(ins, ops[0])
    if m == 'pop':
        if ops[0].size == 4:
            return wr(ins, ops[0], 'CPU_Pop32()')
        if ops[0].type == X86_OP_MEM:
            return '{ const Bit16u v_ = CPU_Pop16(); %s }' % wr(ins, ops[0], 'v_')
        return wr(ins, ops[0], 'CPU_Pop16()')
    if m == 'cbw':
        return 'reg_ax = (Bit16u)(Bit16s)(Bit8s)reg_al;'
    if m == 'cwde':
        return 'reg_eax = (Bit32u)(Bit32s)(Bit16s)reg_ax;'
    if m == 'cwd':
        return 'reg_dx = (reg_ax & 0x8000) ? 0xFFFF : 0;'
    if m == 'cdq':
        return 'reg_edx = (reg_eax & 0x80000000u) ? 0xFFFFFFFFu : 0;'
    if m == 'not':
        n = ops[0].size
        if ops[0].type == X86_OP_REG:
            return '%s = (%s)~%s;' % (reg(ops[0].reg), TY[n], reg(ops[0].reg))
        return '{ const PhysPt a_ = %s; %s(a_, (%s)~%s(a_)); }' % (ea(ins, ops[0]), WR[n], TY[n], RD[n])
    if m == 'nop':
        return ';'
    if m.startswith('set') and ('j' + m[3:]) in CC:
        return wr(ins, ops[0], '(%s) ? 1 : 0' % CC['j' + m[3:]])
    if m == 'stc':
        return '{ FillFlags(); SETFLAGBIT(CF, true); }'
    if m == 'clc':
        return '{ FillFlags(); SETFLAGBIT(CF, false); }'
    if m == 'cld':
        return '{ SETFLAGBIT(DF, false); cpu.direction = 1; }'
    # --- arithmetic through DOSBox's macros
    two = {'add': 'ADD', 'or': 'OR', 'adc': 'ADC', 'sbb': 'SBB', 'and': 'AND', 'sub': 'SUB', 'xor': 'XOR', 'cmp': 'CMP', 'test': 'TEST'}
    if m in two:
        n = ops[0].size
        pre, o1, ld, sv = rm(ins, ops[0], None)
        s = rd(ins, ops[1])
        if ops[1].type == X86_OP_IMM and ops[1].size != n:
            s = '(%s)(%s)%d' % (TY[n], STY[n], ops[1].imm)
        if m in ('cmp', 'sub'):
            flag_src = ('sub', n)
        elif m in ('and', 'or', 'xor', 'test'):
            flag_src = ('logic', n)
        elif m == 'add':
            flag_src = ('res', n)
        return '{ %sconst %s s_ = %s; %s%s(%s, s_, %s, %s) }' % (pre, TY[n], s, two[m], SUF[n], o1, ld, sv)
    if m in ('inc', 'dec'):
        n = ops[0].size
        pre, o1, ld, sv = rm(ins, ops[0], None)
        flag_src = ('res', n)
        return '{ %s%s%s(%s, %s, %s) }' % (pre, m.upper(), SUF[n], o1, ld, sv)
    if m == 'neg':
        n = ops[0].size
        l = LF[n]
        pre, o1, ld, sv = rm(ins, ops[0], None)
        flag_src = ('res', n)
        return '{ %slflags.type = t_NEG%s; lf_var1%s = %s(%s); lf_res%s = 0 - lf_var1%s; %s(%s, lf_res%s); }' % (pre, l, l, ld, o1, l, l, sv, o1, l)
    shifts = {'rol': 'ROL', 'ror': 'ROR', 'rcl': 'RCL', 'rcr': 'RCR', 'shl': 'SHL', 'sal': 'SHL', 'shr': 'SHR', 'sar': 'SAR'}
    if m in shifts:
        n = ops[0].size
        pre, o1, ld, sv = rm(ins, ops[0], None)
        count = rd(ins, ops[1]) if len(ops) > 1 else '1'
        # (the macros leave by `break` when the count is nothing: GRP2 of instructions.h)
        return 'do { %sconst Bit8u val = (Bit8u)(%s) & 0x1f; %s%s(%s, val, %s, %s) } while (0);' % (pre, count, shifts[m], SUF[n], o1, ld, sv)
    if m in ('shld', 'shrd'):
        n = ops[0].size
        pre, o1, ld, sv = rm(ins, ops[0], None)
        return 'do { %sconst %s s_ = %s; D%s%s(%s, s_, (Bit8u)(%s), %s, %s) } while (0);' % (
            pre, TY[n], rd(ins, ops[1]), m[:3].upper(), SUF[n], o1, rd(ins, ops[2]), ld, sv)
    if m in ('mul', 'div', 'idiv') or (m == 'imul' and len(ops) == 1):
        n = ops[0].size
        if ops[0].type == X86_OP_REG:
            o1, ld = reg(ops[0].reg), 'RC_LR'
            pre = ''
        else:
            pre, o1, ld = 'const PhysPt a_ = %s; ' % ea(ins, ops[0]), 'a_', RD[n]
        return '{ rc_at = 0x%04X; %s%s%s(%s, %s, 0) }' % (a, pre, m.upper(), SUF[n], o1, ld)
    if m == 'imul':
        n = ops[0].size
        s1 = rd(ins, ops[1])
        s2 = rd(ins, ops[2]) if len(ops) > 2 else rd(ins, ops[0])
        if len(ops) > 2 and ops[2].type == X86_OP_IMM:
            s2 = '(%s)%d' % (STY[n], ops[2].imm if ops[2].imm < (1 << (8 * n - 1)) else ops[2].imm - (1 << (8 * n)))
        return '{ const %s x_ = %s; const %s y_ = (%s)(%s); DIMUL%s(%s, x_, y_, RC_LR, RC_SR) }' % (TY[n], s1, TY[n], TY[n], s2, SUF[n], reg(ops[0].reg))
    # --- strings (the count and the pointers 16 bits wide)
    if m in ('scasb', 'scasw'):
        n = ops[0].size if ops else (1 if m == 'scasb' else 2)
        acc = {1: 'reg_al', 2: 'reg_ax'}[n]
        if ins.addr_size != 2:
            raise Unsupported('wide string')
        flag_src = ('sub', n)
        return '{ const %s s_ = %s(SegPhys(es) + reg_di); reg_di = (Bit16u)(reg_di + cpu.direction * %d); CMP%s(%s, s_, RC_LR, RC_SR) }' % (TY[n], RD[n], n, SUF[n], acc)
    if m in ('lodsb', 'lodsw'):
        n = 1 if m == 'lodsb' else 2
        if ins.addr_size != 2:
            raise Unsupported('wide string')
        srcseg = 'ds'
        for x in b[:npre]:
            if x in (0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65):
                srcseg = {0x26: 'es', 0x2E: 'cs', 0x36: 'ss', 0x3E: 'ds', 0x64: 'fs', 0x65: 'gs'}[x]
        return '{ %s = %s(SegPhys(%s) + reg_si); reg_si = (Bit16u)(reg_si + cpu.direction * %d); }' % ({1: 'reg_al', 2: 'reg_ax'}[n], RD[n], srcseg, n)
    if m in ('stosb', 'stosw', 'stosd', 'rep stosb', 'rep stosw', 'rep stosd', 'movsb', 'movsw', 'rep movsb', 'rep movsw', 'rep movsd'):
        if ins.addr_size != 2:
            raise Unsupported('wide string')
        rep = m.startswith('rep')
        kind = m.split()[-1]
        n = {'b': 1, 'w': 2, 'd': 4}[kind[-1]]
        acc = {1: 'reg_al', 2: 'reg_ax', 4: 'reg_eax'}[n]
        srcseg = 'ds'
        for x in b[:npre]:
            if x in (0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65):
                srcseg = {0x26: 'es', 0x2E: 'cs', 0x36: 'ss', 0x3E: 'ds', 0x64: 'fs', 0x65: 'gs'}[x]
        if kind.startswith('stos'):
            body = '%s(SegPhys(es) + reg_di, %s); reg_di = (Bit16u)(reg_di + cpu.direction * %d);' % (WR[n], acc, n)
        else:
            body = '%s(SegPhys(es) + reg_di, %s(SegPhys(%s) + reg_si)); reg_si = (Bit16u)(reg_si + cpu.direction * %d); reg_di = (Bit16u)(reg_di + cpu.direction * %d);' % (
                WR[n], RD[n], srcseg, n, n)
        if rep:
            return 'for (; reg_cx; reg_cx--) { %s }' % body
        return '{ %s }' % body
    raise Unsupported(m)


# the patched stretches: whole instructions of the code as shipped that a patch touches -> the same bytes patched, decoded
patch_at = {}
for pa, (was, now) in sorted(PATCHES.items()):
    covering = [a for (s, a) in addrs if s == PSEG and a < pa + len(was) and a + seen[(s, a)].size > pa]
    if not covering:
        continue
    s, e = covering[0], covering[-1] + seen[(PSEG, covering[-1])].size
    data = bytearray(img[PSEG * 16 + s:PSEG * 16 + e])
    if s in patch_at:
        data = patch_at[s][2]
    data[pa - s:pa - s + len(now)] = now
    patch_at[s] = (e, pa, data, now[0])
    for (sg, a) in addrs:
        if sg == PSEG and s < a < e and (sg, a) in targets:
            sys.exit('a jump into the patched stretch at %04X' % a)
for s, (e, pa, data, first) in patch_at.items():
    targets.add((PSEG, e))
    targets.add((PSEG, s))
patched_bytes = set()
for pa, (was, now) in PATCHES.items():
    for k in range(len(was)):
        patched_bytes.add(pa + k)
returns = set((s, RETURN_AT.get((s, a), a)) for (s, a) in returns if (s, a) not in RETURN_DROP)
for x in returns:
    if x not in seen:
        sys.exit('a return place is not an instruction: %04X:%04X' % x)
    targets.add(x)
# the hooked places are arrived at through the dispatch too (a hook sends the CPU there)
for (s, a) in addrs:
    if a in HOOK_IPS:
        targets.add((s, a))

# the C++ functions: each segment's instructions in address order, cut every CHUNK or so where control does not fall through
chunks = []
for seg in sorted(set(s for s, a in addrs)):
    mine = [a for s, a in addrs if s == seg]
    cur = []
    for i, a in enumerate(mine):
        cur.append(a)
        ins = seen[(seg, a)]
        ends = ins.mnemonic in ('jmp', 'ret', 'retf', 'iret', 'int3', 'hlt')
        inpatch = any(s < a + ins.size < e for s, (e, pa, d, f) in patch_at.items()) if seg == PSEG else False
        if (len(cur) >= CHUNK and ends and not inpatch) or len(cur) >= 2 * CHUNK and not inpatch:
            chunks.append((seg, cur))
            cur = []
    if cur:
        chunks.append((seg, cur))
for k, (seg, mine) in enumerate(chunks):
    for a in mine:
        chunk_of[(seg, a)] = k

EXITS = ('jmp', 'ret', 'retf', 'iret', 'int3', 'hlt')
bodies = []
for k, (seg, mine) in enumerate(chunks):
    cur_seg, cur_chunk = seg, k
    out = []
    flag_src = None
    for a in mine:
        ins = seen[(seg, a)]
        lab = 'L_%04X: ' % a if (seg, a) in targets else '        '
        hook = 'RC_HOOK(0x%04X) ' % a if a in HOOK_IPS else ''
        if seg == PSEG and a in patch_at:
            e, pa, data, first = patch_at[a]
            out.append('L_%04X: if (mem_readb_inline(SegPhys(cs) + 0x%04X) == 0x%02X) {      // the patch of wcnet_tie640.cpp is in' % (a, pa, first))
            saved = targets
            targets = set()
            for pins in md.disasm(bytes(data), a):
                flag_src = None
                out.append('            RC_T(0x%04X) %-100s // %04X %s %s (patched)' % (pins.address, translate(pins), pins.address, pins.mnemonic, pins.op_str))
            targets = saved
            flag_src = None
            out.append('            %s' % goto(e))
            out.append('        }')
            lab = '        '
            targets.discard((seg, a))
            try:
                text = translate(ins)
            finally:
                targets.add((seg, a))
        else:
            try:
                text = translate(ins)
            except Unsupported as e:
                unsupported['%s' % e] += 1
                flag_src = None
                text = '{ ip_ = 0x%04X; return -1; }   // not translated: the interpreter\'s' % a
        out.append(lab + hook + 'RC_T(0x%04X) ' % a + '%-110s // %04X %s %s' % (text, a, ins.mnemonic, ins.op_str))
        nxt = a + ins.size
        if ins.mnemonic not in EXITS and chunk_of.get((seg, nxt)) != k:
            out.append('        %s' % goto(nxt))
    bodies.append(out)


# the checksum: FNV-1a over the instructions' bytes in address order, relocated words and patched bytes as zeros; then the tables
def fnv(data, h=0x811C9DC5):
    for x in data:
        h = ((h ^ x) * 0x01000193) & 0xFFFFFFFF
    return h

runs = []       # [segment, offset, length] of the stretches hashed
h = 0x811C9DC5
skip = []
for (seg, a) in addrs:
    ins = seen[(seg, a)]
    cur_seg = seg
    data = bytearray(ins.bytes)
    for r in has_reloc(ins):
        data[r] = data[r + 1] = 0
        skip.append((seg, a + r))
        skip.append((seg, a + r + 1))
    for kk in range(ins.size):
        if seg == PSEG and a + kk in patched_bytes:
            data[kk] = 0
            skip.append((seg, a + kk))
    h = fnv(data, h)
    if runs and runs[-1][0] == seg and runs[-1][1] + runs[-1][2] == a:
        runs[-1][2] += ins.size
    else:
        runs.append([seg, a, ins.size])
for (seg, tt), n in sorted(TABLES.items()):
    h = fnv(img[seg * 16 + tt:seg * 16 + tt + 2 * n], h)
    runs.append([seg, tt, 2 * n])
for seg, tt, n in DS_TABLES:
    h = fnv(img[DGROUP * 16 + tt:DGROUP * 16 + tt + 2 * n], h)
    runs.append([DGROUP, tt, 2 * n])

print('// GENERATED by scripts/tie640-recompile.py from TIEMISS.EXE (the TIE Fighter demo, "V1.0 12/04/93"): do not edit.')
print('// %d instructions of image paragraphs %s, in %d functions.' % (len(addrs), ', '.join('%04X' % s for s in sorted(set(s for s, a in addrs))), len(chunks)))
if unsupported:
    print('// not translated: %s' % dict(unsupported))
print('#ifndef RC_BODY_ONLY')
print('static const Bit32u kRcHash = 0x%08Xu;' % h)
print('static const Bit16u kRcRuns[][3] = { %s };' % ', '.join('{ 0x%04X, 0x%04X, %d }' % (s, a, n) for s, a, n in runs))
print('static const Bit32u kRcSkip[] = { %s };' % ', '.join('0x%08Xu' % ((s << 16) | a) for s, a in sorted(set(skip))))
print('static const Bit32u kRcEntries[] = { %s };' % ', '.join('0x%08Xu' % ((s << 16) | a) for s, a in ENTRIES))
print('static const Bit32u kRcNear[] = { %s };      // entries called near' % ', '.join('0x%08Xu' % ((s << 16) | a) for s, a in NEAR_ENTRIES))
print('static const Bit32u kRcReturns[] = { %s };   // where control comes back into the translation' % ', '.join('0x%08Xu' % ((s << 16) | a) for s, a in sorted(returns) if (s, a) in seen))
print('static const Bit16u kRcHookIps[] = { %s };' % ', '.join('0x%04X' % x for x in sorted(HOOK_IPS)))
# every instruction's offset (any segment), for the check that no hooked place is passed unseen
print('static const Bit16u kRcCode[] = { %s };' % ', '.join('0x%04X' % x for x in sorted(set(a for s, a in addrs))))
# the places the dispatch knows: (segment << 16 | offset) ascending, and each one's function
tl = sorted((s, a) for (s, a) in targets if (s, a) in seen)
print('static const Bit32u kRcPlaces[] = { %s };' % ', '.join('0x%08Xu' % ((s << 16) | a) for s, a in tl))
print('static const Bit8u kRcPlaceFn[] = { %s };' % ', '.join('%d' % chunk_of[x] for x in tl))
print('static int rc_find(Bit16u para, Bit16u ip) {')
print('    const Bit32u key = ((Bit32u)para << 16) | ip;')
print('    const Bit32u *end = kRcPlaces + sizeof(kRcPlaces) / sizeof(kRcPlaces[0]), *at = std::lower_bound(kRcPlaces, end, key);')
print('    return at != end && *at == key ? kRcPlaceFn[at - kRcPlaces] : -1;')
print('}')
print('#endif')
for k, (seg, mine) in enumerate(chunks):
    print('static int RC_FN(%d)(Bit16u &ip_, const Bit16u load) {     // %04X:%04X..%04X' % (k, seg, mine[0], mine[-1]))
    print('    Bit16u ip = ip_, rc_at = 0; (void)rc_at; (void)load;')
    print('rc_dispatch:')
    print('    switch (ip) {')
    for a in mine:
        if (seg, a) in targets:
            print('    case 0x%04X: goto L_%04X;' % (a, a))
    print('    default: ip_ = ip; return rc_find(0x%04X, ip);' % seg)
    print('    }')
    for line in bodies[k]:
        print(line)
    print('    goto rc_fault;')
    print('rc_fault:')
    print('    ip_ = rc_at;        // (a divide the CPU faults on: the interpreter runs it)')
    print('    return -3;')
    print('}')
print('typedef int (*RcFn)(Bit16u &, const Bit16u);')
print('static const RcFn RC_TABLE[] = { %s };' % ', '.join('RC_FN(%d)' % k for k in range(len(chunks))))
sys.stderr.write('%d instructions in %d functions, %d places, returns %s, unsupported %s\n' % (
    len(addrs), len(chunks), len(tl), ['%04X:%04X' % x for x in sorted(returns) if x in seen], dict(unsupported)))
missing = [t for t in targets if t not in seen]
if missing:
    sys.stderr.write('targets not decoded: %s\n' % ['%04X:%04X' % t for t in missing])
