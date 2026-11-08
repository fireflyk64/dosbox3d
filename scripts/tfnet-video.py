#!/usr/bin/env python3
"""TIE Fighter flown together, in one vertical video (1080x1920): the host's picture on top, the guest's below,
each machine's recording (WCLAB=<dir>: rec.bin, src/cpu/wcnet_lab.h) lined up by the wall clock both carry ("wall"
events), so both halves show the same moment.

    tfvideo.py HOST GUEST OUT.mp4 --seg FROM:TO[:host|guest[:CAPTION]] [--seg ...] [--title T] [--sub S]
               [--hook TEXT] [--end TEXT] [--host-label L] [--guest-label L]

FROM/TO: the host's recording's seconds.  Each segment's sound is the host's or the guest's machine's (default the
host's); segments are cut together (the sound crossfaded 40 ms).  --hook: big words over the first 2.5 s; --end: the
last 2.5 s ('|' breaks a line).  Captions sit between the two pictures (a phone's app covers the screen's bottom).

The takes (docs/tiefighter.md, step 15's videos): scripts/tfnet-duo.sh with HOSTENV="WCLAB=<dir>" GUESTENV="WCLAB=<dir>"
and each pilot's !tiesteer=; scripts/tfnet-sheet.py finds the moments.  Needs numpy, Pillow, ffmpeg.
"""
import argparse, os, subprocess, sys, wave
import numpy as np
from PIL import Image, ImageDraw, ImageFont
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'walkthrough'))
from labrec import Take

FONTS = os.path.expanduser('~/.cache/wcwalk/fonts')
def font(name, size):
    p = os.path.join(FONTS, name)
    return ImageFont.truetype(p if os.path.exists(p) else '/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf', size)

def wall_map(take):
    e = [(ms, ev['ms']) for ms, ev in take.events if ev.get('t') == 'wall']
    return np.array([a for a, _ in e], float), np.array([b for _, b in e], float)

W, H, VW, VH = 1080, 1920, 1080, 810
TOP, GAP = 135, 70          # title band, the band between the two pictures
Y1, Y2 = TOP, TOP + VH + GAP

def outlined(d, xy, text, f, fill, anchor='mm', stroke=4):
    d.text(xy, text, font=f, fill=fill, anchor=anchor, stroke_width=stroke, stroke_fill=(0, 0, 0))

def wrap(d, text, f, width):
    if '|' in text:     # (a line break of one's own)
        return [l for part in text.split('|') for l in wrap(d, part.strip(), f, width)]
    words, lines, cur = text.split(), [], ''
    for w in words:
        t = (cur + ' ' + w).strip()
        if d.textlength(t, font=f) > width and cur:
            lines.append(cur); cur = w
        else:
            cur = t
    if cur: lines.append(cur)
    return lines

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('host'); ap.add_argument('guest'); ap.add_argument('out')
    ap.add_argument('--seg', action='append', required=True)
    ap.add_argument('--title', default='TIE Fighter, flown together')
    ap.add_argument('--sub', default='')
    ap.add_argument('--host-label', default='PILOT 1 · the host')
    ap.add_argument('--guest-label', default='PILOT 2 · the guest')
    ap.add_argument('--hook', default='')
    ap.add_argument('--end', default='')
    ap.add_argument('--fps', type=int, default=30)
    a = ap.parse_args()
    h, g = Take(a.host), Take(a.guest)
    hE, hW = wall_map(h); gE, gW = wall_map(g)
    takes = {'host': h, 'guest': g}
    segs = []
    for s in a.seg:
        p = s.split(':', 3)
        segs.append((float(p[0]), float(p[1]), p[2] if len(p) > 2 and p[2] else 'host', p[3] if len(p) > 3 else ''))
    fT, fS, fL, fC, fHook = (font('ChakraPetch-SemiBold.ttf', 50), font('IBMPlexSans.ttf', 28), font('ChakraPetch-SemiBold.ttf', 30),
                             font('ChakraPetch-SemiBold.ttf', 46), font('ChakraPetch-Bold.ttf', 76))
    base = Image.new('RGB', (W, H), (6, 8, 14))
    d = ImageDraw.Draw(base)
    d.text((W // 2, 52), a.title, font=fT, fill=(235, 235, 240), anchor='mm')
    if a.sub:
        d.text((W // 2, 104), a.sub, font=fS, fill=(165, 172, 190), anchor='mm')
    # the sound: each segment's machine, guest moments by the wall clock too
    rate = h.rate
    chunks, frames_plan = [], []
    for s0, s1, who, cap in segs:
        if who == 'host':
            snd = h.sound(s0 * 1000, s1 * 1000)
        else:
            g0 = np.interp(np.interp(s0 * 1000, hE, hW), gW, gE)
            snd = g.sound(g0, g0 + (s1 - s0) * 1000)
        n = int(round((s1 - s0) * rate))
        snd = snd[:n].astype(np.float32)
        if len(snd) < n:
            snd = np.vstack([snd, np.zeros((n - len(snd), 2), np.float32)])
        chunks.append(snd)
        for i in range(int(round((s1 - s0) * a.fps))):
            frames_plan.append((s0 + i / a.fps, cap))
    x = int(0.04 * rate)
    out = chunks[0]
    for c in chunks[1:]:
        ramp = np.linspace(0, 1, x)[:, None]
        mix = out[-x:] * (1 - ramp) + c[:x] * ramp
        out = np.vstack([out[:-x], mix, c[x:]])
    fade = int(0.6 * rate)
    ramp = np.linspace(0, 1, fade)[:, None]
    out[:fade] *= ramp; out[-fade:] *= ramp[::-1]
    wav = a.out + '.wav'
    with wave.open(wav, 'wb') as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(rate)
        w.writeframes(np.clip(out, -32768, 32767).astype(np.int16).tobytes())
    ff = subprocess.Popen(['ffmpeg', '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', f'{W}x{H}', '-r', str(a.fps),
                           '-i', '-', '-i', wav, '-c:v', 'libx264', '-preset', 'medium', '-crf', '19', '-pix_fmt', 'yuv420p',
                           '-c:a', 'aac', '-b:a', '192k', '-shortest', '-movflags', '+faststart', a.out], stdin=subprocess.PIPE)
    total = len(frames_plan)
    for i, (hs, cap) in enumerate(frames_plan):
        t = i / a.fps
        hm = hs * 1000
        gm = np.interp(np.interp(hm, hE, hW), gW, gE)
        frame = base.copy()
        for take, ms, y in ((h, hm, Y1), (g, gm, Y2)):
            pic = Image.fromarray(take.picture(ms))
            frame.paste(pic.resize((VW, VH), Image.LANCZOS if pic.size[0] >= 640 else Image.NEAREST), (0, y))
        dd = ImageDraw.Draw(frame)
        mid = Y1 + VH + GAP // 2
        big = (a.hook and t < 2.5) or (a.end and t > total / a.fps - 2.5)
        if cap and not big:
            # (between the two pictures, where the eye is: a phone's app covers the screen's bottom)
            lines = wrap(dd, cap, fC, W - 80)
            for k, line in enumerate(lines):
                outlined(dd, (W // 2, mid + (k - (len(lines) - 1) / 2) * 56), line, fC, (255, 236, 150), stroke=5)
        elif not big:
            dd.text((24, mid), a.host_label + '  ▲', font=fL, fill=(110, 195, 255), anchor='lm')
            dd.text((W - 24, mid), '▼  ' + a.guest_label, font=fL, fill=(255, 185, 100), anchor='rm')
        for text, on in ((a.hook, a.hook and t < 2.5), (a.end, a.end and t > total / a.fps - 2.5)):
            if on:
                lines = wrap(dd, text, fHook, W - 100)
                y0 = Y1 + VH + GAP // 2 - (len(lines) - 1) * 44
                for k, line in enumerate(lines):
                    outlined(dd, (W // 2, y0 + k * 88), line, fHook, (255, 255, 255), stroke=7)
        arr = np.asarray(frame, np.float32)
        k = min(1.0, (t + 1 / a.fps) / 0.5, (total / a.fps - t) / 0.5)
        if k < 1:
            arr *= max(0.0, k)
        ff.stdin.write(arr.astype(np.uint8).tobytes())
    ff.stdin.close(); ff.wait(); os.remove(wav)
    print(f'{a.out}: {total} frames, {total / a.fps:.1f} s')

if __name__ == '__main__':
    main()
