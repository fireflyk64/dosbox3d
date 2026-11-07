#!/usr/bin/env python3
# The ship's address system's call to the briefing room, for TIE Fighter's guests (docs/tiefighter.md, step 13): the
# emulator plays it itself (src/cpu/wcnet_tie_pa.cpp), TIE Fighter having no sound of ours to play.  It is RetroVega's
# own `pa_report` (retrovega/tools/sfx.py: a two-note chime and "All pilots, report to the briefing room." in our own
# synthesized voice, art/voice/pa_report.wav, through a speaker's band and a hall's echo), rendered by that tool and
# halved to 11,025 Hz (the band ends at 3,600 Hz), written as src/cpu/wcnet_tie_pa.h.
#
#   scripts/tf-pa-sound.py        (run from the repository; needs numpy, as sfx.py does)
import os, subprocess, sys, tempfile, wave
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
with tempfile.TemporaryDirectory() as d:
    subprocess.check_call([sys.executable, os.path.join(ROOT, "retrovega", "tools", "sfx.py"), "--wav", d],
                          cwd=os.path.join(ROOT, "retrovega"), stdout=subprocess.DEVNULL)
    with wave.open(os.path.join(d, "pa_report.wav")) as w:
        assert w.getsampwidth() == 1 and w.getnchannels() == 1, "pa_report.wav: 8-bit mono expected"
        rate = w.getframerate()
        u8 = w.readframes(w.getnframes())
s = [b - 128 for b in u8]
half = [round((s[i] + s[i + 1]) / 2) for i in range(0, len(s) - 1, 2)]    # (each pair's mean: the halving's low pass)
out = os.path.join(ROOT, "src", "cpu", "wcnet_tie_pa.h")
with open(out, "w") as f:
    f.write("/* Made by scripts/tf-pa-sound.py from RetroVega's pa_report (retrovega/tools/sfx.py): do not edit. */\n")
    f.write("#ifndef WCNET_TIE_PA_H_\n#define WCNET_TIE_PA_H_\n")
    f.write("static const unsigned kPaRate = %d;\nstatic const unsigned kPaSamples = %d;\n" % (rate // 2, len(half)))
    f.write("static const signed char kPaSound[%d] = {\n" % len(half))
    for i in range(0, len(half), 24):
        f.write("    " + ",".join(str(v) for v in half[i:i + 24]) + ",\n")
    f.write("};\n#endif\n")
print("%s: %d samples at %d Hz, %.2f s" % (out, len(half), rate // 2, len(half) / (rate / 2)))
