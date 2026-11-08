# loadsum.py DIR: for each machine's run.err, the near heap's lowest point and the most puppets, deaths, errors
import sys, re, glob, os
for f in sorted(glob.glob(os.path.join(sys.argv[1], '*', 'run.err'))):
    txt = open(f, 'rb').read().decode('latin-1')
    low, start, peak = None, None, (0, 0, 0)
    for m in re.finditer(r't=([\d.]+), \d+ keyboard frames.*?near heap free (\d+) \(lowest this second (-?\d+), ever (-?\d+).*?ours here: (\d+) pilots, (\d+) encounter ships, (\d+) pods', txt):
        t, free, lows, lowe, p, n, pd = float(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)), int(m.group(6)), int(m.group(7))
        if start is None: start = free
        if lows >= 0 and (low is None or lows < low[0]): low = (lows, t, p, n, pd)
        if p + n + pd > sum(peak): peak = (p, n, pd)
    name = os.path.basename(os.path.dirname(f))
    print('%-8s loads=%d ended=%d errors=%d deaths=%d noroom=%d | free at first %s, lowest %s at t=%s with %s pilots %s ships %s pods | most puppets %s' % (
        name, txt.count('PRCD.EXE loaded'), txt.count('PRCD.EXE ended'), txt.count('reports error'), txt.count('our ship was destroyed'),
        txt.count('no room'), start, low[0] if low else '-', low[1] if low else '-', low[2] if low else '-', low[3] if low else '-',
        low[4] if low else '-', '%d+%d+%d' % peak))
    for m in re.finditer(r'reports error[^\n]*', txt): print('   ', m.group(0)[:150])
    for m in re.finditer(r'our ship was destroyed[^\n]*', txt): print('   ', m.group(0)[:150])
