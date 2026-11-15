#!/usr/bin/env python3
"""Convert a single-geometry COLLADA polylist (Maya OpenCOLLADA export) to a Wavefront OBJ (tools/anaxander.sh: Vega
Strike's Anaxander, whose Maya source is in Assets-Masters and was never converted to .bfxm; bfxm.py reads the .obj).

    python3 -I dae2obj.py IN.dae OUT.obj [MTLNAME TEXTURE]
Keeps positions, normals and texture coordinates; polygons are kept as they are
(quads stay quads).  The node transform is identity in the Anaxander file; any
other is reported, not applied.  Units are left as in the file (centimetres)."""
import sys
import xml.etree.ElementTree as ET

NS = '{http://www.collada.org/2005/11/COLLADASchema}'


def main():
    src, out = sys.argv[1], sys.argv[2]
    mtl = sys.argv[3] if len(sys.argv) > 3 else None
    tex = sys.argv[4] if len(sys.argv) > 4 else None
    root = ET.parse(src).getroot()
    arrays = {}
    for fa in root.iter(NS + 'float_array'):
        arrays[fa.get('id')] = [float(v) for v in fa.text.split()]
    sources = {}
    for s in root.iter(NS + 'source'):
        fa = s.find(NS + 'float_array')
        if fa is None:
            continue
        acc = s.find('.//' + NS + 'accessor')
        stride = int(acc.get('stride')) if acc is not None else 3
        vals = arrays[fa.get('id')]
        sources['#' + s.get('id')] = [vals[i:i + stride] for i in range(0, len(vals), stride)]
    for v in root.iter(NS + 'vertices'):
        inp = v.find(NS + 'input')
        sources['#' + v.get('id')] = sources[inp.get('source')]
    for node in root.iter(NS + 'node'):
        for t in node:
            if t.tag in (NS + 'translate', NS + 'rotate', NS + 'scale', NS + 'matrix'):
                print('node', node.get('id'), t.tag.split('}')[1], t.text)
    lines = ['# converted from %s by dae2obj.py' % src.split('/')[-1]]
    if mtl:
        lines.append('mtllib %s.mtl' % mtl)
    voff = noff = toff = 0
    nfaces = 0
    for geom in root.iter(NS + 'geometry'):
        mesh = geom.find(NS + 'mesh')
        for pl in list(mesh.findall(NS + 'polylist')) + list(mesh.findall(NS + 'triangles')):
            inputs = pl.findall(NS + 'input')
            stride = max(int(i.get('offset')) for i in inputs) + 1
            by = {i.get('semantic'): (int(i.get('offset')), sources[i.get('source')]) for i in inputs}
            P = by['VERTEX'][1]
            N = by.get('NORMAL', (None, []))[1]
            T = by.get('TEXCOORD', (None, []))[1]
            for p in P:
                lines.append('v %g %g %g' % tuple(p[:3]))
            for n in N:
                lines.append('vn %g %g %g' % tuple(n[:3]))
            for t in T:
                lines.append('vt %g %g' % tuple(t[:2]))
            if mtl:
                lines.append('usemtl %s' % mtl)
            idx = [int(x) for x in pl.find(NS + 'p').text.split()]
            vc = pl.find(NS + 'vcount')
            counts = [int(x) for x in vc.text.split()] if vc is not None else [3] * (len(idx) // (3 * stride))
            k = 0
            for c in counts:
                corners = []
                for _ in range(c):
                    rec = idx[k:k + stride]
                    k += stride
                    v = rec[by['VERTEX'][0]] + 1 + voff
                    t = rec[by['TEXCOORD'][0]] + 1 + toff if 'TEXCOORD' in by else ''
                    n = rec[by['NORMAL'][0]] + 1 + noff if 'NORMAL' in by else ''
                    corners.append('%d/%s/%s' % (v, t, n))
                lines.append('f ' + ' '.join(corners))
                nfaces += 1
            voff += len(P)
            noff += len(N)
            toff += len(T)
    with open(out, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    if mtl:
        with open(out[:-4] + '.mtl', 'w') as f:
            f.write('newmtl %s\nKa 0 0 0\nKd 1 1 1\nKs 0.5 0.5 0.5\n' % mtl)
            if tex:
                f.write('map_Kd %s\n' % tex)
    print(out, voff, 'vertices', nfaces, 'faces')


main()
