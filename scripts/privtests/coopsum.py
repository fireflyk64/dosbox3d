# coopsum.py DIR: per machine, over time: pirates in its scene, our puppets, the heap's low
import sys, re, glob, os
rows = {}
for f in sorted(glob.glob(os.path.join(sys.argv[1], '*', 'run.err'))):
    name = os.path.basename(os.path.dirname(f))
    txt = open(f, 'rb').read().decode('latin-1')
    for m in re.finditer(r't=([\d.]+), \d+ keyboard frames.*?lowest this second (-?\d+).*?ours here: (\d+) pilots, (\d+) encounter ships.*?ships in the scene (\d+), pirates (\d+)', txt):
        t = int(float(m.group(1)))
        if t % 5: continue
        rows.setdefault(t, {})[name] = '%s/%s/%s/%s' % (m.group(6), m.group(3), m.group(4), (int(m.group(2)) // 100) if int(m.group(2)) >= 0 else '-')
names = sorted({n for r in rows.values() for n in r}, key=lambda n: (n != 'host', n))
print('t     ' + ' '.join('%-14s' % n for n in names) + '   (pirates seen / pilots shown / encounter puppets / heap low in 100 B)')
for t in sorted(rows):
    print('%-5d ' % t + ' '.join('%-14s' % rows[t].get(n, '') for n in names))
