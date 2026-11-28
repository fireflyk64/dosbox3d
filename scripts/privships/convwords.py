#!/usr/bin/env python3
"""The spoken words of Privateer's conversations and when each is said (docs/privateer.md section 42): what the
added ships' sale scripts (src/cpu/wcnet_priv_convs.inc) are cut from.

  convwords.py <PRIV.TRE> <outdir> CONV [CONV ...]     # e.g. S7MBC4 BUYFIGHT CREDITS

Writes <outdir>/<CONV>_<line>.wav (a line's speech: DATA\\CONV\\<CONV>.VPK's entry, the game's LZW undone, the
Creative Voice File's samples, 8 bits at 11,111 a second) and prints each line's written text (the .PFC's) and, if
the faster-whisper package is there (pip install faster-whisper; the model is fetched once), the words heard with
their times in seconds -- "said[from-to]".  The written and the spoken differ where the text has a number.
"""
import os, struct, subprocess, sys, wave
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "retrovega", "tools"))
from wcformat import lzw_decode

def tre_files(path):
    d = open(path, "rb").read()
    n = struct.unpack_from("<I", d, 0)[0]
    out = {}
    for i in range(n):
        e = d[8 + 74 * i:8 + 74 * (i + 1)]
        off, size = struct.unpack_from("<II", e, 66)
        out[e[1:66].split(b"\0")[0].decode("latin1").upper()] = d[off:off + size]
    return out

def pfc_lines(d):
    """The records: (0, speaker, mood, backdrop, four bytes, text, last byte), (7, speaker, backdrop, four bytes), (5,), (8,)"""
    p, out = 0, []
    name = lambda q: d[q:q + 9].split(b"\0")[0].decode("latin1")
    while p < len(d):
        k = d[p]; p += 1
        if k in (5, 8): out.append((k,)); continue
        who = name(p); p += 9
        mood = None
        if k == 0: mood = name(p); p += 9
        place = name(p); p += 9
        look = tuple(d[p:p + 4]); p += 4
        if k == 7: out.append((k, who, place, look)); continue
        q = d.index(b"\0", p)
        out.append((k, who, mood, place, look, d[p:q].decode("latin1"), d[q + 1])); p = q + 2
    return out

def vpk_lines(d):
    first = struct.unpack_from("<I", d, 4)[0] & 0xFFFFFF
    offs = [struct.unpack_from("<I", d, 4 + 4 * i)[0] for i in range((first - 4) // 4)]
    for i, o in enumerate(offs):
        end = (offs[i + 1] & 0xFFFFFF) if i + 1 < len(offs) else len(d)
        b = d[o & 0xFFFFFF:end]
        yield lzw_decode(b[4:], struct.unpack_from("<I", b, 0)[0]) if o >> 24 == 0x20 else b

def voc_samples(b):
    assert b[:19] == b"Creative Voice File"
    p, out, rate = struct.unpack_from("<H", b, 20)[0], b"", 11111
    while p < len(b) and b[p] != 0:
        n = b[p + 1] | b[p + 2] << 8 | b[p + 3] << 16
        body = b[p + 4:p + 4 + n]
        if b[p] == 1: rate = round(1000000 / (256 - body[0])); out += body[2:]
        elif b[p] == 2: out += body
        p += 4 + n
    return rate, out

def main():
    tre, outdir, convs = sys.argv[1], sys.argv[2], sys.argv[3:]
    files = tre_files(tre)
    os.makedirs(outdir, exist_ok=True)
    try:
        import numpy as np
        from faster_whisper import WhisperModel
        model = WhisperModel("small.en", device="cpu", compute_type="int8")
    except ImportError:
        model = None
    for conv in convs:
        key = "..\\..\\DATA\\CONV\\" + conv.upper()
        lines = [r for r in pfc_lines(files[key + ".PFC"]) if r[0] == 0]
        for i, (line, voc) in enumerate(zip(lines, vpk_lines(files[key + ".VPK"]))):
            rate, pcm = voc_samples(bytes(voc))
            path = os.path.join(outdir, "%s_%02d.wav" % (conv.upper(), i))
            w = wave.open(path, "wb"); w.setnchannels(1); w.setsampwidth(1); w.setframerate(rate); w.writeframes(pcm); w.close()
            print("%s#%d %s (%s, %s): %s" % (conv.upper(), i, line[1], line[2], line[3], line[5]))
            if model:
                raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-ar", "16000", "-ac", "1", "-f", "s16le", "-"], capture_output=True).stdout
                segs, _ = model.transcribe(np.frombuffer(raw, np.int16).astype(np.float32) / 32768.0, word_timestamps=True, condition_on_previous_text=False)
                print("    said: " + " ".join("%s[%.2f-%.2f]" % (w.word.strip(), w.start, w.end) for s in segs for w in s.words))

if __name__ == "__main__":
    main()
