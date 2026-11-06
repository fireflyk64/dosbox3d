#!/usr/bin/env python3
"""artweb.py: the files a flight asks the page for (RVWANT.TXT: src/ships.c ships_want), as the web pages serve
them (web/rvsets.js): each ship type's views (art/ships/SHIPnn.RV), gzipped, with ships.json (the build, and each
file's size and hash: the page's cache keys).  (The asteroid fields' ROCKS.DAT was served here too until 2026-11-06:
it is in the game's own package now.)
Both pages use it: the DOSBox one (scripts/build-web.sh, into web/dist/rv) and the native build's
(host/web/site.sh, into its art/).

    tools/artweb.py --out DIR --build REV
"""
import argparse
import csv
import gzip
import hashlib
import json
import os
import shutil

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))


def gz(src, dst):
    if os.path.exists(dst) and os.path.getmtime(dst) >= os.path.getmtime(src):
        return
    # (no name or time stamp in the gzip header: the same file, the same bytes)
    with open(src, 'rb') as f, open(dst + '.part', 'wb') as raw, \
            gzip.GzipFile(filename='', mode='wb', compresslevel=9, fileobj=raw, mtime=0) as g:
        shutil.copyfileobj(f, g)
    os.replace(dst + '.part', dst)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--build', required=True)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    names = {r['type'].lower(): r['name'] for r in csv.DictReader(open(os.path.join(ROOT, 'data/ships.csv')))}
    ships = {}
    art = os.path.join(ROOT, 'art/ships')
    for f in sorted(os.listdir(art)):
        if not f.startswith('SHIP') or not f.endswith('.RV'):
            continue
        path = os.path.join(art, f)
        gz(path, os.path.join(a.out, f + '.gz'))
        data = open(path, 'rb').read()
        key = f[:-3]
        ships[key] = {'file': 'rv/' + f + '.gz', 'bytes': len(data), 'sha1': hashlib.sha1(data).hexdigest(),
                      'name': names.get('0x' + key[4:6].lower(), key)}
    # the game's files a flight asks for as they are (none since 2026-11-06: the asteroid fields' ROCKS.DAT is in the
    # game's own package, build-web.sh's retrovega.tar.gz and host/web/site.sh's game.tar.gz; web/rvsets.js still
    # serves any listed here)
    files = {}
    for f in []:
        path = os.path.join(ROOT, 'build/dos', f)
        gz(path, os.path.join(a.out, f + '.gz'))
        data = open(path, 'rb').read()
        files[f] = {'file': 'rv/' + f + '.gz', 'bytes': len(data), 'sha1': hashlib.sha1(data).hexdigest()}
    # (a file served here before and not now, ROCKS.DAT.gz for one, gone: a deploy copies the directory as it is)
    served = {e['file'][3:] for e in list(ships.values()) + list(files.values())}
    for f in os.listdir(a.out):
        if f.endswith('.gz') and f not in served:
            os.remove(os.path.join(a.out, f))
    json.dump({'build': a.build, 'ships': ships, 'files': files}, open(os.path.join(a.out, 'ships.json'), 'w'), indent=1)
    print('%s/ships.json: %d ships, %.1f MB of views (%.1f MB gzipped)' % (
        a.out, len(ships), sum(s['bytes'] for s in ships.values()) / 1e6,
        sum(os.path.getsize(os.path.join(a.out, s['file'][3:])) for s in ships.values()) / 1e6))


if __name__ == '__main__':
    main()
