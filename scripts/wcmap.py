#!/usr/bin/env python3
"""wcmap.py -- carry what is known about one Wing Commander executable over to
another (WC.EXE -> WC2.EXE, WC2.EXE -> SO1.EXE, ...).

The games share an engine, relinked and grown.  A function keeps its shape
(the sequence of instructions, with addresses blanked), so:

  1. every function of the old executable is paired with the most similar
     function of the new one (bigrams of instruction tokens, as wcexe.py's
     `match`), keeping pairs that are each other's best match;
  2. paired functions are aligned instruction by instruction, and wherever
     two aligned instructions differ only in a data address or a constant,
     that is a vote "old address X is new address Y".

  scripts/wcmap.py wc/WC.EXE wc2/WC2.EXE funcs  [addr ...]   # where did these functions go (default: all pairs)
  scripts/wcmap.py wc/WC.EXE wc2/WC2.EXE data   [XXXX ...]   # votes for data-segment offsets (default: wcnet_ds.def's)
  scripts/wcmap.py wc/WC.EXE wc2/WC2.EXE def                 # wcnet_ds.def lines with the winning offset and its votes

The pairing is cached next to the scratch directory given by $WCMAP_CACHE
(default /tmp/wcmap-<names>.pickle); delete it after changing this script.
"""
import collections
import difflib
import os
import pickle
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wcexe

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ADDR = re.compile(r"(?<=\[)0x[0-9a-f]{1,4}(?=\])|0x[0-9a-f]{3,4}\b")


def functions(x):
    """[(seg, start, end, tokens, raw instruction texts)] for every function."""
    out = []
    for seg in x.code_segments():
        starts = x.funcs(seg)
        code, _ = x.seg_bytes(seg)
        for i, s in enumerate(starts):
            e = starts[i + 1] if i + 1 < len(starts) else len(code)
            if e - s < 12:
                continue
            raw = [text for _o, _h, text in x.ndisasm(code[s:e], s)]
            toks = []
            for text in raw:
                t = re.sub(r"0x[0-9a-f]+:(?:word )?0x[0-9a-f]+|word 0x[0-9a-f]+:word 0x[0-9a-f]+", "FAR", text)
                t = re.sub(r"\[[^\]]*0x[0-9a-f]{3,}[^\]]*\]", "[M]", t)
                t = re.sub(r"\[0x[0-9a-f]+\]", "[M]", t)   # a global, however low its address
                t = re.sub(r"\b(call|jmp short|jmp|j[a-z]+|loop)\s+0x[0-9a-f]+", r"\1 L", t)
                t = re.sub(r"0x[0-9a-f]{3,}", "N", t)
                toks.append(t)
            out.append((seg, s, e, toks, raw))
    return out


def grams(toks):
    g = collections.Counter()
    for i in range(len(toks) - 1):
        g[(toks[i], toks[i + 1])] += 1
    return g


def pair(old, new):
    """Mutual best matches: {index in old: (index in new, score)}."""
    og = [grams(f[3]) for f in old]
    ng = [grams(f[3]) for f in new]
    on = [sum(g.values()) for g in og]
    nn = [sum(g.values()) for g in ng]
    by_len = sorted(range(len(new)), key=lambda j: nn[j])
    lens = [nn[j] for j in by_len]
    import bisect

    def best(i, src_g, src_n, dst_g, dst_n, order, order_lens):
        if src_n[i] < 6:
            return None, 0.0
        lo = bisect.bisect_left(order_lens, src_n[i] * 0.55)
        hi = bisect.bisect_right(order_lens, src_n[i] * 1.8 + 4)
        top, top_j = 0.0, None
        g = src_g[i]
        for j in order[lo:hi]:
            h = dst_g[j]
            common = sum(min(n, h.get(k, 0)) for k, n in g.items())
            score = 2.0 * common / (src_n[i] + dst_n[j])
            if score > top:
                top, top_j = score, j
        return top_j, top

    fwd = {}
    for i in range(len(old)):
        j, score = best(i, og, on, ng, nn, by_len, lens)
        if j is not None:
            fwd[i] = (j, score)
    by_len_o = sorted(range(len(old)), key=lambda i: on[i])
    lens_o = [on[i] for i in by_len_o]
    out = {}
    for i, (j, score) in fwd.items():
        back, _ = best(j, ng, nn, og, on, by_len_o, lens_o)
        if back == i:
            out[i] = (j, score)
    return out


def votes(old, new, pairs, min_score=0.45):
    """{old constant: Counter(new constant)} from aligned instruction pairs."""
    v = collections.defaultdict(collections.Counter)
    for i, (j, score) in pairs.items():
        if score < min_score:
            continue
        a, b = old[i], new[j]
        sm = difflib.SequenceMatcher(None, a[3], b[3], autojunk=False)
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            if tag != "equal":
                continue
            for k in range(i2 - i1):
                ra, rb = a[4][i1 + k], b[4][j1 + k]
                if "FAR" in a[3][i1 + k] or re.match(r"(call|j\w+|loop)\b", ra):
                    continue
                ca, cb = ADDR.findall(ra), ADDR.findall(rb)
                if len(ca) != len(cb):
                    continue
                for p, q in zip(ca, cb):
                    pa, qa = int(p, 16), int(q, 16)
                    # [bx-0x42e6] is dseg:BD1A + bx
                    if "-" + p in ra:
                        pa = (0x10000 - pa) & 0xFFFF
                    if "-" + q in rb:
                        qa = (0x10000 - qa) & 0xFFFF
                    v[pa][qa] += 1
    return v


def load(old_path, new_path):
    cache = os.environ.get("WCMAP_CACHE") or "/tmp/wcmap-%s-%s.pickle" % (
        os.path.basename(old_path).lower(), os.path.basename(new_path).lower())
    stamp = (os.path.getmtime(old_path), os.path.getmtime(new_path), os.path.getmtime(__file__))
    if os.path.exists(cache):
        got = pickle.load(open(cache, "rb"))
        if got.get("stamp") == stamp:
            return got
    xo, xn = wcexe.Exe(old_path), wcexe.Exe(new_path)
    old, new = functions(xo), functions(xn)
    pairs = pair(old, new)
    got = dict(stamp=stamp, old=old, new=new, pairs=pairs, votes=dict(votes(old, new, pairs)))
    pickle.dump(got, open(cache, "wb"))
    return got


def main(argv):
    if len(argv) < 4:
        print(__doc__)
        return 1
    old_path, new_path, cmd = argv[1], argv[2], argv[3]
    m = load(old_path, new_path)
    old, new, pairs, v = m["old"], m["new"], m["pairs"], m["votes"]
    xo, xn = wcexe.Exe(old_path), wcexe.Exe(new_path)
    if cmd == "funcs":
        want = set()
        for a in argv[4:]:
            seg, off = xo.parse(a)
            want.add((seg, xo.func_bounds(seg, off)[0]))
        idx = dict(((f[0], f[1]), i) for i, f in enumerate(old))
        rows = sorted(idx.items()) if not want else [(w, idx.get(w)) for w in sorted(want)]
        for (seg, start), i in rows:
            if i is None or i not in pairs:
                if want:
                    print("%s  (no confident counterpart)" % xo.label(seg, start))
                continue
            j, score = pairs[i]
            n = new[j]
            exported = ""
            if n[0].startswith("ovr"):
                e = [so for so, t in xn.ovr[int(n[0][3:])]["entries"] if t == n[1]]
                exported = "  thunk stub%s:%04X" % (n[0][3:], e[0]) if e else "  (not exported)"
            print("%s -> %s  %.2f  %d/%d bytes%s" % (xo.label(seg, start), xn.label(n[0], n[1]), score,
                                                    old[i][2] - old[i][1], n[2] - n[1], exported))
    elif cmd in ("data", "def"):
        defs = []
        path = os.path.join(ROOT, "src", "cpu", "wcnet_ds.def")
        for line in open(path):
            mm = re.match(r"WC_DS\((\w+), (0x[0-9A-Fa-f]+), (0x[0-9A-Fa-f]+)\)(.*)$", line.rstrip("\n"))
            defs.append((line.rstrip("\n"), mm))
        if cmd == "data":
            offs = [int(a, 16) for a in argv[4:]] or [int(mm.group(2), 16) for _l, mm in defs if mm]
            names = dict((int(mm.group(2), 16), mm.group(1)) for _l, mm in defs if mm)
            for o in offs:
                c = v.get(o, {})
                top = collections.Counter(c).most_common(4)
                print("%04X %-28s %s" % (o, names.get(o, ""), "  ".join("%04X x%d" % (q, n) for q, n in top) or "-"))
        else:
            for line, mm in defs:
                if not mm:
                    print(line)
                    continue
                c = collections.Counter(v.get(int(mm.group(2), 16), {})).most_common(2)
                best = c[0] if c else (0, 0)
                second = c[1][1] if len(c) > 1 else 0
                sure = best[1] >= 3 and best[1] >= 2 * second
                print("WC_DS(%s, %s, 0x%04X)%s    /* votes %d vs %d%s */" % (
                    mm.group(1), mm.group(2), best[0] if sure else 0, mm.group(4), best[1], second, "" if sure else " UNSURE %04X" % best[0]))
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
