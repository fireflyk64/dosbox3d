"""The third draft's last cruise pieces, a second rock field and a second launch (CC0, ours), in Outbound's style
(tools/flightsongs.py): a fast motor under a tune in long notes, a few parts each with its own job, keys that move
by thirds, hits where a groove would be, runs and sweeps, a riser into the loop, blocks marked for the score.

The user's soundtrack review (2026-11-17) kept Wide Orbit, Sensor Sweep, Deep Lane, Starwake and the rock field,
and asked for more of each kind, and a real launch:

    CRUISE9   "Apogee"        F sharp minor, 12/8 at 120: Wide Orbit's vein ("randomly select from more of
                              these"): the harp climbing and falling over a turning spiccato, the horn singing,
                              round the major-third cycle F sharp minor, D, B flat; a harp sweep on C sharp
    CRUISE10  "Faint Signal"  A minor, 152: searching, in Sensor Sweep's spirit ("more variety: this is good"):
                              open fifths stacked in 3+3+2, the horn reaching up in fifths over roots falling by
                              thirds; the signal found in C, lifted to E flat; home through B flat to E; the
                              harp and piccolo strums mostly up, a few down, one an A and one a V
    CRUISE11  "Nightside"     C sharp minor, 146: dark wonder, Deep Lane's vein ("another variation to play flying
                              around"): falling ripples in 3+3+2, one phrase sinking a major third twice (C sharp
                              minor, A minor, F minor), whole-tone flourishes, home through the Neapolitan D; all
                              that a faint background now, under a trumpet's climb in triplets ("something very
                              different but in the same genre on the foreground")
    CRUISE12  "Starfall"      E minor, 150: Starwake's laments and very fast runs in turn ("less chiming...
                              and maybe some inverses"): its lament, its drive, its carillon and its runs turned
                              upside down; the chimes only in the intro and the riser
    ROCKS2    "Drift Field"   D dorian, 118: the second asteroid and mine field song ("more specific to
                              asteroids with them swooshing by too as exploration happens"): a drifting tune, and
                              rocks going by, string whooshes, harp glissandi and timpani rumbles, panned across;
                              every whoosh the one the user liked ("more of a swooshing sound when they pass by")
    LAUNCH2   "Catapult"      C minor into E, 156: the flight deck, the catapult, the thrust out into space

The cruise cycle (src/score.c plays them in turn, each riser on its own dominant, the next piece's top in its own
key; tools/cruise_b.py): Starwake (D minor, its riser on A), Apogee (F sharp minor, riser on C sharp), Faint Signal
(A minor, on E), Nightside (C sharp minor, on G sharp), Starfall (E minor, on B), then Outbound's E minor.

The orchestra is orch.py's at its measured balance.  Drift Field pans copies of three of its parts (the spiccato,
the harp, the timpani, at their levels) for the rocks going by: the chip puts a voice left, in both or right as the
note starts, so a run moves across in three steps.  (The noise swell's patch, program 119 in tools/oplbank.py, is
not reachable: the timpani's patch, written after it, takes 119 too, and the bank plays 119 as the timpani.)
"""
from compose import chord
from orch import (DRUM, ORCH, Piece, in_time, up, scale_run, ACC332, orchestra, in_range, octaves, pulses, pedal332, hit,
                  held, sung, sweep, MAJOR, LYDIAN)

THIRD, SIXTH = 1 / 3, 1 / 6             # in 12/8 (four dotted beats a bar): an eighth, a sixteenth
MINOR = (0, 2, 3, 5, 7, 8, 10)          # (a run's steps from its first note)
HARMONIC = (0, 2, 3, 5, 7, 8, 11)
MIXOLYDIAN = (0, 2, 4, 5, 7, 9, 10)
PHRYGIAN_DOM = (0, 1, 4, 5, 7, 8, 10)   # (the fifth's scale in harmonic minor: C sharp, D, E sharp, F sharp ...)
WHOLE = (0, 2, 4, 6, 8, 10)
PAN_L, PAN_C, PAN_R = 16, 64, 112       # (src/opl.c: under 43 the left, over 85 the right, between both)


def chans(s):
    """the parts' channels: horn, horn2, motor, low, hit, brass, piccolo, timpani, harp, strings, bass"""
    ch = orchestra(s)
    return tuple(ch[k] for k in ('horn', 'horn2', 'motor', 'low', 'hit', 'brass', 'piccolo', 'timpani', 'harp',
                                 'strings', 'bass'))


def scale_up(lo, root, steps, count):
    """so many notes of a scale (its root's pitch class, the steps from it) from lo upward"""
    pcs = {(root + k) % 12 for k in steps}
    out, q = [], lo
    while len(out) < count:
        if q % 12 in pcs:
            out.append(q)
        q += 1
    return out


def ladder(c, q0, n=12):
    """the chord's tones upward from q0 (its bass): q0, then each pitch of the chord above it"""
    pcs = {(c.root + t) % 12 for t in c.tones} | {c.bass}
    out, q = [q0], q0 + 1
    while len(out) < n:
        if q % 12 in pcs:
            out.append(q)
        q += 1
    return out


def sweep_down(s, ch, bar, pcs, lo, hi, vel, beats=None):
    """orch.sweep the other way: a harp's strum down through these pitch classes, hi to lo, in the bar's last
    so many beats, growing into the bar line (each note let go by the bar's end: a loop stays whole bars)"""
    beats = beats or s.beats
    notes = [q for q in range(hi, lo - 1, -1) if q % 12 in pcs]
    step = beats / len(notes)
    for k, q in enumerate(notes):
        at = s.beats - beats + k * step
        s.play(ch, bar, at, min(step * 1.5, s.beats - 0.02 - at), q, vel + k, 2)


def bar_beat(s, pos):
    return int(pos // s.beats + 1e-9), pos - s.beats * int(pos // s.beats + 1e-9)


def root_in(s, bar, lo=33, hi=44):
    return in_range(chord(s.sym_at(bar, 0)).root, lo, hi)


# --- CRUISE9: "Apogee" (F sharp minor, 12/8 at 120) -----------------------------------
# Wonder at the top of an orbit, in Wide Orbit's vein with its own tune and its own wheel.  The spiccato turns like
# a wheel (each dotted beat the bass, the fifth and the octave going up, then over the top and down from the tenth
# or the twelfth), and the harp climbs the chord in a zigzag of eighths for a bar and falls for the next, round and
# round.  The horn sings a fifth up and holds; the four blocks go round the major-third cycle: F sharp minor with
# the bass falling (F sharp, E, D, C sharp), D (the deceptive turn), B flat (F's lift into it), and back through
# D, B minor and the Neapolitan G to C sharp: the harp sweeps up C sharp's chord into the top.  The tune rises a
# fifth, a sixth, a fifth again in turn and comes down through the last block.  The second pass has the tune on
# the strings in octaves with the horn's line under it, hits on the first and third dotted beats, the horn with
# the second horn under it and the piccolo over its end, a brass chord under the last block.

AP_A = ['F#m', 'F#m/E', 'Dmaj7', 'C#7sus4 C#']
AP_B = ['D', 'Em7', 'Gmaj7', 'F']
AP_C = ['Bb', 'Gm9', 'Ebmaj7', 'D']
AP_D = ['Bm9', 'Gmaj7', 'C#7sus4 C#', 'C#']
AP_PASS = AP_A + AP_B + AP_C + AP_D
AP_TUNE = [['F#4:0:2 C#5:2:2', 'E5:0:3 C#5:3:1', 'A4:0:2 B4:2:1 C#5:3:1', 'B4:0:2 G#4:2:2'],
           ['A4:0:2 F#5:2:2', 'G5:0:3 E5:3:1', 'D5:0:2 B4:2:1 D5:3:1', 'C5:0:2 A4:2:2'],
           ['Bb4:0:2 F5:2:2', 'A5:0:3 G5:3:1', 'Eb5:0:2 F5:2:1 G5:3:1', 'A5:0:2 F#5:2:2'],
           ['F#5:0:3 C#5:3:1', 'D5:0:2 B4:2:1 F#4:3:1', 'B4:0:2 G#4:2:1 F4:3:1', 'G#4:0:4']]
AP_UNDER = [['A3:0:4', 'A3:0:4', 'F#3:0:4', 'F#3:0:2 F3:2:2'], ['F#3:0:4', 'G3:0:4', 'G3:0:2 F#3:2:2', 'F3:0:4'],
            ['F3:0:4', 'F3:0:4', 'G3:0:4', 'F#3:0:4'], ['D4:0:4', 'D4:0:4', 'F#3:0:2 F3:2:2', 'F3:0:4']]
AP_HARP = [0, 2, 1, 3, 2, 4, 3, 5, 4, 6, 5, 7, 8, 6, 7, 5, 6, 4, 5, 3, 4, 2, 3, 1]     # up a bar, down the next


def wheel12(s, ch, bars, vel, lo=42):
    """12/8: the spiccato turning like a wheel: each dotted beat three eighths up the chord from its bass, the
    first and third beats rising (bass, fifth, octave), the second and fourth falling from the top (tenth or
    twelfth, octave, fifth); the beats' first notes louder"""
    shapes = ((0, 2, 3), (4, 3, 2), (0, 2, 3), (5, 3, 2))
    for b in bars:
        for beat in range(4):
            c = chord(s.sym_at(b, beat))
            lad = ladder(c, in_range(c.bass, lo, lo + 11))
            for k, i in enumerate(shapes[beat]):
                s.play(ch, b, beat + k * THIRD, 0.28, lad[i], vel + (10 if k == 0 else 0) + (4 if beat == 0 else 0), 3)


def apogee():
    form = ['F#m'] + AP_PASS + AP_PASS
    s = Piece(120, 2909, form, level=1.0)
    hn, hn2, mo, lo, hi_, br, pc, tp, hp, st, bs = chans(s)
    n = len(form)                                   # 1 + 16 + 16 = 33 bars, 66 s
    s.loop_at(1)
    s.arp(hp, range(0, n), AP_HARP, THIRD, 52, 64, 3, 48, accent=6, gate=0.6)
    wheel12(s, mo, range(0, n), 56)
    s.ramp(mo, 0, 1, 50, 127)                       # the intro: the wheel and the harp coming up
    s.ramp(hp, 0, 1, 60, 127)
    s.play(tp, 0, 0, 1, 'F#2', 76)
    s.bass(bs, range(1, n), [(0, 2.9, 'r'), (3, 0.9, '5')], 72, lo=30, hi=42)
    for P, second in ((1, False), (17, True)):
        A, B, C, D = P, P + 4, P + 8, P + 12
        R = D + 3
        for b in (A, B, C, D):
            s.sect(b)
        if not second:
            sung(s, hn, A, AP_TUNE[0], 80)
            s.play(tp, A, 0, 1, 'F#2', 84)
            s.crash(B, 0, 100)
            hit(s, hi_, B, 0, 100, timpani=tp)
            sung(s, st, B, AP_TUNE[1], 80)
            s.run(pc, B + 3, 2, scale_up(77, 5, MIXOLYDIAN, 10), SIXTH, 58, 2)     # F up, into B flat
            sung(s, st, C, AP_TUNE[2], 82)
            s.tune(hn, C, [up(t, -12) for t in AP_TUNE[2]], 64)
            hit(s, hi_, C, 0, 98, timpani=tp)
            s.run(pc, C + 3, 2, scale_up(78, 2, MAJOR, 10), SIXTH, 58, 2)          # D from F sharp up
            sung(s, st, D, AP_TUNE[3], 84)
            s.tune(hn, D, [up(t, -12) for t in AP_TUNE[3][:3]], 70)
            s.crash(D, 0, 104)
            for b in (D, D + 2):
                hit(s, hi_, b, 0, 104, timpani=tp)
        else:
            for blk in (A, C, D):
                s.crash(blk, 0, 106)
                for bb in (blk, blk + 2):
                    for beat in (0, 2):
                        hit(s, hi_, bb, beat, 104, timpani=tp if beat == 0 else None)
            sung(s, st, A, [up(t, 12) for t in AP_TUNE[0]], 78)
            s.tune(st, A, AP_TUNE[0], 70)
            sung(s, hn, A, AP_UNDER[0], 68)
            s.run(pc, A + 3, 2, scale_up(73, 1, PHRYGIAN_DOM, 10), SIXTH, 58, 2)  # C sharp up, into D
            sung(s, hn, B, AP_TUNE[1], 82)
            sung(s, hn2, B, AP_UNDER[1], 64)
            s.tune(pc, B + 2, [up(t, 12) for t in AP_TUNE[1][2:]], 50)
            hit(s, hi_, B, 0, 100, timpani=tp)
            sung(s, st, C, AP_TUNE[2], 80)
            s.tune(st, C, [up(t, -12) for t in AP_TUNE[2]], 70)
            sung(s, hn, C, AP_UNDER[2], 68)
            s.run(pc, C + 3, 2, scale_up(78, 2, MAJOR, 10), SIXTH, 60, 2)
            sung(s, st, D, AP_TUNE[3], 84)
            sung(s, hn2, D, AP_UNDER[3], 66)
            s.pad(br, range(D, D + 3), 50, 64, 3, 60)
        # the riser: the harp up C sharp's chord in the last two beats, the timpani rolling up
        sweep(s, hp, R, {1, 5, 8}, 49, 85, 58 if second else 52, beats=2)
        s.roll(tp, R, 0, 4, 'C#2', 8, 96 if second else 74, step=SIXTH)
    s.end(n)
    return s


# --- CRUISE10: "Faint Signal" (A minor, 152) -----------------------------------------
# Searching, in Sensor Sweep's spirit with its own sound.  The spiccato stacks open fifths (the root, its fifth,
# its ninth) in 3+3+2 eighths, a pulse that keeps looking, while the roots fall by thirds (A minor, F, D minor,
# B flat, G, E minor, C) and the horn reaches up in fifths in long notes (A to E, B over F's lydian sharp four, A to
# E an octave up, G to D), never settling, to E's suspension.  Then the signal: the harp sweeps up E's chord and C
# major opens (a third away), the horn's tune with the hits over broken chords, lifted a minor third to E flat
# (the piccolo running up G into it), round through A flat and B flat; and the search's pulse again under the
# strings' way home through F, D minor and B flat to E, the horns swelling on E over a timpani roll into the top.
# The second pass has the search on the strings in octaves with the horn's guide tones under them, the hits and a
# brass chord; the tune on the strings in octaves, the hits on every accent.

FS_SEARCH = ['Am9', 'Fmaj7#11', 'Dm9', 'Bbmaj7#11', 'Gsus4 G', 'Em7', 'Cmaj7', 'Esus4 E']
FS_FOUND = ['C', 'Em/B', 'Fmaj7', 'G', 'Eb', 'Gm/D', 'Abmaj7', 'Bbsus4 Bb']
FS_HOME = ['Fmaj7', 'Dm9', 'Bbmaj7#11', 'E7sus4 E']
FS_PASS = FS_SEARCH + FS_FOUND + FS_HOME + ['E', 'E']
FS_SEARCH_TUNE = ['A3:0:1 E4:1:3', 'B4:0:4', 'A4:0:1 E5:1:3', 'D5:0:3 E5:3:1', 'D5:0:2 B4:2:2', 'G4:0:1 D5:1:3',
                  'E5:0:3 B4:3:1', 'A4:0:2 G#4:2:2']
FS_SEARCH_UNDER = ['C4:0:4', 'A3:0:4', 'F3:0:4', 'A3:0:4', 'C4:0:2 B3:2:2', 'B3:0:4', 'G3:0:4', 'B3:0:4']
FS_FOUND_TUNE = ['G4:0:2 C5:2:2', 'B4:0:3 G4:3:1', 'A4:0:2 C5:2:1 E5:3:1', 'D5:0:4', 'Bb4:0:2 Eb5:2:2',
                 'D5:0:3 Bb4:3:1', 'C5:0:2 Eb5:2:1 G5:3:1', 'F5:0:2 D5:2:2']
FS_FOUND_UNDER = ['E4:0:4', 'G3:0:4', 'A3:0:4', 'B3:0:4', 'G3:0:4', 'Bb3:0:4', 'Ab3:0:4', 'F3:0:4']
FS_HOME_TUNE = ['C5:0:3 A4:3:1', 'F5:0:2 E5:2:1 D5:3:1', 'F5:0:3 E5:3:1', 'D5:0:2 B4:2:1 G#4:3:1']
FS_HOME_UNDER = ['A3:0:4', 'A3:0:4', 'D4:0:4', 'B3:0:2 G#3:2:2']


def sonar(s, ch, bars, vel, lo=45):
    """the motor: the root, its fifth and its ninth in 3+3+2 eighths (open fifths stacked: a sound still
    looking), the root of each group louder"""
    for b in bars:
        for k in range(8):
            beat = k * 0.5
            c = chord(s.sym_at(b, beat))
            q = in_range(c.root, lo, lo + 11)
            fifth = 7 if 7 in c.tones else 6
            q += (0, fifth, 14, 0, fifth, 14, 0, fifth)[k]
            s.play(ch, b, beat, 0.42, q, vel + (10 if beat in ACC332 else 0) - (4 if k in (2, 5) else 0), 3)


def faint_signal():
    form = ['Am9', 'Am9'] + FS_PASS + FS_PASS
    s = in_time(Piece(152, 3010, form, level=0.5))
    hn, hn2, mo, lo, hi_, br, pc, tp, hp, st, bs = chans(s)
    n = len(form)                                   # 2 + 22 + 22 = 46 bars, 72.6 s
    s.loop_at(2)
    sonar(s, mo, range(0, 2), 56)                   # the intro: the pulse coming up, the low strings
    s.ramp(mo, 0, 2, 40, 127)
    pedal332(s, lo, range(1, 2), 78)
    s.play(tp, 0, 0, 1, 'A1', 70)
    s.roll(tp, 1, 2, 2, 'E2', 20, 76, step=0.125)
    for P, second in ((2, False), (24, True)):
        S1, S2, F1, F2, H, R = P, P + 4, P + 8, P + 12, P + 16, P + 20
        for b in (S1, S2, F1, F2, H):
            s.sect(b)
        # the search: the pulse, the low strings, the horn reaching up (the strings' in octaves the second time)
        sonar(s, mo, range(S1, F1), 62)
        pedal332(s, lo, range(S1, F1), 84)
        s.play(tp, S1, 0, 1, 'A1', 82)
        s.play(tp, S2, 0, 1, 'G1', 78)
        if not second:
            sung(s, hn, S1, FS_SEARCH_TUNE, 80, e0=56, e1=124, e2=92)
            s.arp(hp, range(S2, F1), [0, 1, 2, 3, 4, 3, 2, 1], 0.5, 60, 84, 4, 42, gate=0.6)
        else:
            s.crash(S1, 0, 100)
            for b in (S1, S1 + 2):
                for beat in (0, 1.5):
                    hit(s, hi_, b, beat, 104, timpani=tp if beat == 0 else None)
            sung(s, st, S1, [up(t, 12) for t in FS_SEARCH_TUNE], 78, e0=56, e1=124, e2=92)
            s.tune(st, S1, FS_SEARCH_TUNE, 70)
            sung(s, hn, S1, FS_SEARCH_UNDER, 64)
            s.pad(br, range(S2, F1), 54, 68, 3, 54)
            s.arp(hp, range(S2, F1), [0, 1, 2, 3, 4, 5, 4, 3], 0.5, 60, 84, 4, 46, gate=0.6)
        # (the user, 2026-11-17: "what if that first harp goes down instead of up once or twice to surprise the
        # listener? or even have a A or V shape once or twice": the strums all went up.  Now the first falls,
        # G down to G onto G's chord, the harp's sweep into the signal falls the second time, and the turns home
        # from the signal are an A the first time and a V the second)
        if not second:
            s.run(pc, S1 + 3, 2, scale_up(79, 10, LYDIAN, 8)[::-1], 0.25, 70, -2)  # B flat lydian, G down to G
            sweep(s, hp, S2 + 3, {4, 8, 11}, 40, 88, 56, beats=2)                 # found: the harp up E's chord
        else:
            s.run(pc, S1 + 3, 2, scale_up(77, 10, LYDIAN, 8), 0.25, 56, 2)      # B flat lydian, F up to F
            sweep_down(s, hp, S2 + 3, {4, 8, 11}, 40, 88, 56, beats=2)            # found: the harp down E's chord
        s.roll(tp, S2 + 3, 2, 2, 'E2', 20, 90, step=0.125)
        # the signal: C major, the horn's tune (the strings' in octaves the second time), the hits; E flat
        s.arp(mo, range(F1, H), [0, 2, 4, 2, 1, 3, 5, 3], 0.5, 48, 66, 3, 62)
        pedal332(s, lo, range(F1, H), 90)
        s.bass(bs, range(F1, H), [(0, 3.9, 'r')], 74, lo=31, hi=43)
        s.crash(F1, 0, 108)
        s.crash(F2, 0, 104 if second else 96)
        for blk in (F1, F2):
            for b in (blk, blk + 2):
                for beat in (ACC332 if second else (0, 1.5)):
                    hit(s, hi_, b, beat, 110, timpani=tp if beat == 0 else None)
        if not second:
            sung(s, hn, F1, FS_FOUND_TUNE, 82, e0=70, e1=124, e2=100)
        else:
            sung(s, st, F1, [up(t, 12) for t in FS_FOUND_TUNE], 80, e0=70, e1=124, e2=100)
            s.tune(st, F1, FS_FOUND_TUNE, 72)
            sung(s, hn, F1, FS_FOUND_UNDER, 68)
        s.run(pc, F1 + 3, 2, scale_run(79, 91, MAJOR)[:8], 0.25, 58, 2)            # G up to G, into E flat
        if not second:                                  # B flat's F up to D and back to B flat: an A
            s.run(pc, F2 + 3, 2, [77, 79, 81, 82, 84, 86], 0.25, 60, 2)
            s.run(pc, F2 + 3, 3.5, [84, 82], 0.25, 68, -2)
        else:
            # the piccolo's echo of the tune's end, its D the V's top (the piccolo's notes written in time order:
            # a note written before an earlier one at its pitch cut that one to a tick, and the echo, written
            # before the run into E flat, had lost its C and its G)
            s.tune(pc, F2 + 2, [up(t, 12) for t in FS_FOUND_TUNE[6:7]] + ['F6:0:2'], 50)
            s.run(pc, F2 + 3, 2, [86, 84, 82, 81, 79], 0.25, 62, -1)          # from D down to G, up to C: a V
            s.run(pc, F2 + 3, 3.25, [81, 82, 84], 0.25, 60, 3)
        # home: the search's pulse under the strings, through F, D minor and B flat to E
        sonar(s, mo, range(H, R), 64)
        pedal332(s, lo, range(H, R), 88)
        hit(s, hi_, H, 0, 100, timpani=tp)
        if not second:
            sung(s, st, H, FS_HOME_TUNE, 80, e0=70, e1=120, e2=90)
        else:
            sung(s, st, H, [up(t, 12) for t in FS_HOME_TUNE], 80, e0=70, e1=120, e2=90)
            s.tune(st, H, FS_HOME_TUNE, 68)
        sung(s, hn, H, FS_HOME_UNDER, 64)
        s.run(pc, H + 3, 2, scale_up(76, 9, HARMONIC, 8), 0.25, 60, 2)            # E up, A harmonic minor
        # the riser on E: the motor pulsing, the horns swelling, the timpani rolling up from nothing
        pulses(s, mo, range(R, R + 2), 56)
        s.ramp(mo, R, 1, 80, 127)
        pedal332(s, lo, range(R, R + 2), 88)
        held(s, hn, R, 0, 8, 'B3', 72, e0=40, e1=127, e2=127)
        held(s, hn2, R, 0, 8, 'G#3', 68, e0=40, e1=127, e2=127)
        s.roll(tp, R, 0, 8, 'E2', 6, 110)
        if second:
            sweep(s, hp, R + 1, {4, 8, 11}, 40, 88, 58, beats=2)
    s.end(n)
    return s


# --- CRUISE11: "Nightside" (C sharp minor, 146) --------------------------------------
# Dark wonder, the night side: Deep Lane's vein with its own material.  The spiccato falls in ripples on the
# 3+3+2 (octave, fifth, third; octave, fifth, third; fifth, root) while the low strings strike the bass and the
# harp climbs slowly above.  One phrase (a minor sixth up, then falling by steps) is sung over C sharp minor and
# its Neapolitan D on a C sharp pedal, then again a major third lower on the horn in A minor, then a major third
# lower again but high on the strings in F minor, the darkest block, with the timpani on every accent; each key a
# major third under the last, so the third comes round to where it began: C7 over E slides to C sharp minor over
# the same E, and the payoff climbs through A and the Neapolitan D to G sharp, the horns suspended over it.
# Whole-tone flourishes on the piccolo at the turns.  The second pass: the strings in octaves with the horn under
# them, the horns in octaves, a brass chord under the payoff, the hits on every accent.
#
# (The user, 2026-11-17: "This is fine but seems a bit similar.  Could we have this be a faint background and
# something very different but in the same genre on the foreground".)  So all of that is the background now, 8 dB
# softer (the hits and drums 4: the blocks still land), its tune thinner and out of the way (the strings' octave
# above gone, the F minor and payoff blocks' tune an octave down), the risers coming up from it to full.  Over it,
# the foreground: a trumpet, the colour no other cruise piece leads with, singing one long climb across the pass
# where the old phrase sinks by thirds under it.  Its own rhythm: a long note and three quarter-note triplets
# rising into the next bar's peak, a half step up onto the Neapolitan each time (A over D, B flat over B flat),
# then falling back by the same triplets; its peaks climbing a half step a block (A, B flat, C, C sharp) and its
# phrase starts the C sharp minor chord (C sharp, E, G sharp held over the slide into F minor), to the payoff's
# top C sharp, where the old tune's top was; down to G sharp for the riser.  The second time the brass doubles it
# an octave under from the dark block on.

NS_A = ['C#m', 'Dmaj7/C#', 'F#m/C#', 'G#7/C']
NS_B = ['Am', 'Bbmaj7/A', 'Dm/A', 'E7/G#']
NS_C = ['Fm', 'Gbmaj7/F', 'Bbm/F', 'C7/E']
NS_D = ['C#m/E', 'Amaj7', 'D', 'G#7sus4 G#']
NS_PASS = NS_A + NS_B + NS_C + NS_D + ['G#', 'G#']
NS_TUNE_A = ['G#4:0:1.5 E5:1.5:2.5', 'D5:0:3 C#5:3:1', 'C#5:0:1.5 A4:1.5:.5 F#4:2:2', 'D#5:0:2 C5:2:2']
NS_TUNE_B = [up(t, -4) for t in NS_TUNE_A]
NS_TUNE_C = [up(t, 4) for t in NS_TUNE_A]
NS_TUNE_D = ['C#5:0:1.5 G#5:1.5:2.5', 'G#5:0:2 E5:2:1 C#5:3:1', 'D5:0:2 F#5:2:1 A5:3:1', 'C#6:0:2 C6:2:2']
NS_UNDER_A = ['E4:0:4', 'F#4:0:4', 'C#4:0:4', 'C4:0:2 D#4:2:2']
NS_UNDER_B = [up(t, -4) for t in NS_UNDER_A]
NS_UNDER_C = [up(t, 4) for t in NS_UNDER_A]
NS_UNDER_D = ['E4:0:4', 'E4:0:4', 'F#4:0:4', 'F#4:0:2 D#4:2:2']
T2, T3, TD = '2.66667', '3.33334', '.66667'            # quarter-note triplets on beats 3 and 4
NS_LEAD = [
    'C#5:0:2 E5:2:{d} F#5:{a}:{d} G#5:{b}:{d}', 'A5:0:3 F#5:3:1', 'E5:0:2 F#5:2:{d} E5:{a}:{d} D#5:{b}:{d}',
    'C5:0:2 D#5:2:2',
    'E5:0:2 F5:2:{d} G5:{a}:{d} A5:{b}:{d}', 'Bb5:0:3 A5:3:1', 'F5:0:2 G5:2:{d} F5:{a}:{d} E5:{b}:{d}',
    'D5:0:2 G#5:2:2',
    'Ab5:0:3 G5:3:1', 'F5:0:2 Eb5:2:{d} F5:{a}:{d} Ab5:{b}:{d}', 'Bb5:0:2 Ab5:2:{d} G5:{a}:{d} Bb5:{b}:{d}',
    'C6:0:4',
    'C#6:0:3 B5:3:1', 'C#6:0:2 B5:2:{d} A5:{a}:{d} G#5:{b}:{d}', 'A5:0:3 F#5:3:1', 'G#5:0:4']
NS_LEAD = [t.format(a=T2, b=T3, d=TD) for t in NS_LEAD]
NS_BED_DB, NS_ACCENT_DB = -8.0, -4.0


class Faint(Piece):
    """a piece with a foreground: every other part plays its velocities so many dB softer (softer and mellower:
    the bank's vsense), the hits and the drums less so; shape: (bar, to bar, share at the one, at the other) of
    that softening where it is not all of it (an intro as it was, 0 to 0; a riser coming up to full, 1 to 0)"""

    def __init__(self, bpm, seed, harmony, level, fg, bed_db, accent_db, shape):
        super().__init__(bpm, seed, harmony, level=level)
        self.fg, self.bed_db, self.accent_db, self.shape = set(fg), bed_db, accent_db, shape

    def play(self, ch, bar, beat, dur, pitches, vel, spread=4):
        if ch not in self.fg:
            db = self.accent_db if ch in (9, ORCH['hit'][0]) else self.bed_db
            pos = bar * self.beats + beat
            for b0, b1, f0, f1 in self.shape:
                if b0 * self.beats <= pos < b1 * self.beats:
                    db *= f0 + (f1 - f0) * (pos - b0 * self.beats) / ((b1 - b0) * self.beats)
                    break
            vel = vel * 10 ** (db / 20)
        super().play(ch, bar, beat, dur, pitches, vel, spread)


def falls(s, ch, bars, vel, lo=52):
    """the motor: ripples falling down the chord from the octave on the 3+3+2 (octave, fifth, third; octave,
    fifth, third; fifth, root), the octaves on the accents louder"""
    for b in bars:
        for k in range(8):
            beat = k * 0.5
            c = chord(s.sym_at(b, beat))
            r = in_range(c.root, lo, lo + 11)
            third = 3 if 3 in c.tones else 4 if 4 in c.tones else 5
            fifth = 7 if 7 in c.tones else 6
            d = (12, fifth, third, 12, fifth, third, fifth, 0)[k]
            s.play(ch, b, beat, 0.42, r + d, vel + (12 if beat in ACC332 else 0), 3)


def nightside():
    form = ['C#m', 'C#m'] + NS_PASS + NS_PASS
    ld, ld2 = 12, 13                                # the foreground: the trumpet, the brass under it
    s = in_time(Faint(146, 3111, form, 0.5, (ld, ld2), NS_BED_DB, NS_ACCENT_DB,
                      ((0, 2, 0, 0), (18, 19, 1, 0), (19, 20, 0, 0), (36, 37, 1, 0), (37, 38, 0, 0))))
    hn, hn2, mo, lo, hi_, br, pc, tp, hp, st, bs = chans(s)
    s.setup(ld, 56, 104)
    s.setup(ld2, 61, 92)
    n = len(form)                                   # 2 + 18 + 18 = 38 bars, 62.5 s
    s.loop_at(2)
    pedal332(s, lo, range(0, 2), 84)                # the intro: the low strings, the ripples coming in
    falls(s, mo, range(1, 2), 58)
    s.ramp(mo, 1, 1, 50, 127)
    s.play(tp, 0, 0, 1, 'C#2', 80)
    for P, second in ((2, False), (20, True)):
        A, B, C, D, R = P, P + 4, P + 8, P + 12, P + 16
        for b in (A, B, C, D):
            s.sect(b)
        falls(s, mo, range(A, R), 64)
        pedal332(s, lo, range(A, R), 90)
        s.arp(hp, range(A, C), [0, 1, 2, 3], 1, 64, 84, 4, 46 if second else 42, accent=4, gate=0.6)
        s.arp(hp, range(C, D), [0, 1, 2, 3], 1, 58, 78, 4, 38, accent=4, gate=0.6)   # (lower: the dark block)
        s.arp(hp, range(D, R), [0, 1, 2, 3, 4, 3, 2, 1], 0.5, 64, 84, 4, 46 if second else 42, gate=0.6)
        s.crash(A, 0, 104)
        for blk in ((A, B, D) if second else (A,)):
            for b in (blk, blk + 2):
                for beat in (ACC332 if second else (0, 1.5)):
                    hit(s, hi_, b, beat, 106, timpani=tp if beat == 0 else None)
        if not second:
            s.play(tp, B, 0, 1, 'A1', 76)
        # the darkest block: the timpani with the low strings on every 3+3+2, a hit only on its first beat
        hit(s, hi_, C, 0, 100)
        for b in range(C, D):
            for beat in ACC332:
                s.play(tp, b, beat, 0.6, root_in(s, b), (78 if second else 70) + (8 if beat == 0 else 0), 3)
        # the old tune, under the trumpet, thinner: the strings' high octaves gone, the F minor and payoff blocks
        # an octave down (the horn's octave under the F minor block alone), no horn under the payoff and no brass
        # chord (the trumpet's climb is the payoff now)
        if not second:
            sung(s, hn, A, NS_TUNE_A, 80)
            sung(s, hn, B, NS_TUNE_B, 82)
            sung(s, hn2, B, NS_UNDER_B, 62)
            s.tune(hn, C, [up(t, -12) for t in NS_TUNE_C], 70)
            sung(s, st, D, [up(t, -12) for t in NS_TUNE_D], 82)
        else:
            sung(s, st, A, NS_TUNE_A, 76)
            sung(s, hn, A, NS_UNDER_A, 66)
            s.tune(hn, B, NS_TUNE_B, 82)
            s.tune(hn2, B, [up(t, -12) for t in NS_TUNE_B], 70)
            s.tune(hn, C, [up(t, -12) for t in NS_TUNE_C], 80)
            sung(s, st, D, [up(t, -12) for t in NS_TUNE_D], 84)
        s.crash(D, 0, 108 if second else 100)
        if not second:
            for b in (D, D + 2):
                hit(s, hi_, b, 0, 104, timpani=tp)
        s.run(pc, B + 3, 2, scale_up(76, 4, WHOLE, 10), SIXTH, 58, 2)         # E's whole tones up
        s.run(pc, C + 3, 2, scale_up(76, 0, WHOLE, 10)[::-1], SIXTH, 60, -2)  # and down again over C7
        s.run(pc, D + 2, 2, scale_up(78, 2, LYDIAN, 10), SIXTH, 60, 2)        # D lydian (its G sharp) up
        # the riser: the motor pulsing on G sharp, the horn's suspension resolving, the timpani rolling up
        pulses(s, mo, range(R, R + 2), 58)
        s.ramp(mo, R, 1, 80, 127)
        pedal332(s, lo, range(R, R + 2), 92)
        held(s, hn, R, 0, 4, 'C#4', 72, e0=40, e1=110, e2=110)
        held(s, hn, R + 1, 0, 4, 'C4', 76, e0=100, e1=127, e2=127)
        held(s, hn2, R, 0, 8, 'G#3', 68, e0=40, e1=127, e2=127)
        s.roll(tp, R, 0, 8, 'G#1', 6, 104)
        if second:
            sweep(s, hp, R + 1, {8, 0, 3}, 44, 87, 58, beats=2)
    # the foreground: the trumpet's climb, each pass, growing a little block by block (the brass an octave under
    # it the second time, from F minor)
    for P, second in ((2, False), (20, True)):
        for k in range(4):
            sung(s, ld, P + 4 * k, NS_LEAD[4 * k:4 * k + 4], 98 + 4 * k, e0=96, e1=127, e2=104)
        if second:
            sung(s, ld2, P + 8, [up(t, -12) for t in NS_LEAD[8:]], 84, e0=90, e1=127, e2=104)
    s.end(n)
    return s


# --- CRUISE12: "Starfall" (E minor, 150) ---------------------------------------------
# Starwake's other face: its pensive laments and very fast runs in turn, with far less chiming, and its ideas
# turned upside down.  The lament is Starwake's inverted (each step of its line mirrored about the fifth: where it
# fell it rises, so it ends on the dominant reaching up, not sinking), over a bass falling E, D, C, B, G, F sharp;
# the spiccato's soft sixteenths and the harp's quarters fall where Starwake's rose.  Then the runs: Starwake's
# Andalusian descent climbs here (E, F sharp, G, A under chords in their second inversion), the horns' falling
# fourths become rising ones, its chimes' carillon (rising threes) is the harp's falling threes, its spiccato's
# zigzag goes down, its piccolo's runs up D harmonic minor come down E's, in thirty-seconds.  The lament again a
# major third lower, in C minor (darker, on the strings in octaves with the horn under them), and the runs again,
# wilder, rocking down a half step (E minor and its leading tone's diminished seventh, where Starwake rocked up to
# its Neapolitan), the brass an octave under the strings.  The riser holds B; the chimes toll only there and in
# the intro.

SF_L1 = ['Em', 'Bm/D', 'Am/C', 'Cmaj7', 'Bm', 'Em/G', 'D/F#', 'Bsus4 B']
SF_R1 = ['Am/E', 'Bm/F#', 'C/G', 'D/A', 'Em', 'Cmaj7', 'Am7', 'B7']
SF_L2 = ['Cm', 'Gm/Bb', 'Fm/Ab', 'Abmaj7', 'Gm', 'Cm/Eb', 'Abmaj7', 'B7']
SF_R2 = ['Em', 'D#dim7', 'Em', 'D#dim7', 'C', 'Am', 'F#m7b5', 'B7']
SF_LAMENT = ['B4:0:3 D5:3:1', 'F#4:0:2 G4:2:1 B4:3:1', 'A4:0:3 C5:3:1', 'E5:0:2 G5:2:2', 'F#5:0:1.5 D5:1.5:.5 B4:2:2',
             'G4:0:3 B4:3:1', 'F#4:0:2 G4:2:1 A4:3:1', 'B4:0:4']
SF_LAMENT_UNDER = ['G3:0:4', 'F#3:0:4', 'E3:0:4', 'E3:0:4', 'D3:0:4', 'E3:0:4', 'D3:0:4', 'E3:0:2 D#3:2:2']
SF_LAMENT2 = [up(t, -4) for t in SF_LAMENT[:6]] + ['C5:0:2 Eb5:2:1 G5:3:1', 'F#5:0:2 D#5:2:2']
SF_LAMENT2_UNDER = ['G3:0:4', 'F3:0:4', 'F3:0:4', 'Eb3:0:4', 'D3:0:4', 'Eb3:0:4', 'Eb3:0:4', 'D#3:0:4']
SF_DRIVE = ['E4:0:2 A4:2:2', 'F#4:0:2 B4:2:2', 'G4:0:2 C5:2:2', 'A4:0:3 D5:3:1', 'E5:0:1.5 D5:1.5:.5 B4:2:2',
            'C5:0:1.5 D5:1.5:.5 E5:2:2', 'C5:0:2 E5:2:2', 'F#5:0:2 D#5:2:2']
SF_DRIVE2 = ['G4:0:2 B4:2:2', 'A4:0:2 C5:2:2', 'B4:0:2 E5:2:2', 'C5:0:2 F#5:2:2', 'G5:0:2 E5:2:2', 'E5:0:3 C5:3:1',
             'A4:0:2 C5:2:2', 'F#5:0:2 D#5:2:2']
SF_DRIVE2_HORNS = ['G4:0:4', 'F#4:0:4', 'G4:0:4', 'F#4:0:4', 'E4:0:4', 'E4:0:4', 'E4:0:4', 'D#4:0:4']
FALLING_THREES = [5, 3, 1, 4, 2, 0, 3, 1]               # Starwake's carillon (rising threes) upside down, on the harp
SF_LOW = [(k * .5, .35, w) for k, w in enumerate('r5r8r5r8')]


def starfall():
    form = ['Em', 'Em'] + SF_L1 + SF_R1 + SF_L2 + SF_R2 + ['B', 'B', 'B', 'B']
    s = Piece(150, 3212, form, level=-1.8)
    hn, hn2, mo, lo, hi_, br, pc, tp, hp, st, bs = chans(s)
    cm = 12
    s.setup(cm, 14, 96)                             # the chimes (Starwake's): the intro and the riser only
    n = len(form)                                   # 2 + 32 + 4 = 38 bars, 60.8 s
    P1, F1, P2, F2, R = 2, 10, 18, 26, 34
    s.loop_at(P1)
    for b in range(P1, R, 4):
        s.sect(b)
    # the intro: one toll of E over a low E, the timpani rolling in, the spiccato falling softly
    s.play(cm, 0, 0, 2.0, ['E5', 'E4'], 68)
    s.play(lo, 0, 0, 7.8, 'E2', 74)
    s.roll(tp, 1, 0, 4, 'E2', 10, 70)
    s.arp(mo, range(0, 2), [2, 1, 0, 1], 0.25, 50, 69, 3, 40, accent=6, gate=0.5)
    s.ramp(mo, 0, 2, 40, 127)
    # the laments: the spiccato soft and falling, the low strings holding, the harp falling in quarters
    for P, tune, under, second in ((P1, SF_LAMENT, SF_LAMENT_UNDER, False), (P2, SF_LAMENT2, SF_LAMENT2_UNDER, True)):
        s.arp(mo, range(P, P + 8), [2, 1, 0, 1], 0.25, 50, 69, 3, 46 if second else 40, accent=6, gate=0.5)
        s.bass(lo, range(P, P + 8), [(0, 3.8, 'r')], 78, lo=33, hi=44)
        s.arp(hp, range(P, P + 8), [3, 2, 1, 0], 1, 57, 81, 4, 44, accent=4, gate=0.6)
        s.play(tp, P, 0, 1, root_in(s, P), 72)
        s.play(tp, P + 4, 0, 1, root_in(s, P + 4), 70)
        if not second:
            sung(s, hn, P, tune, 82, e0=60, e1=122, e2=94)
            sung(s, hn2, P + 4, under[4:], 62)
        else:
            sung(s, st, P, tune, 80, e0=60, e1=122, e2=94)
            s.tune(st, P, [up(t, -12) for t in tune], 70)
            sung(s, hn, P, under, 66)
        s.roll(tp, P + 7, 2, 2, 'B1', 20, 92, step=0.125)
    # the runs: the spiccato's zigzag falling, the low strings driving, the harp's falling threes, the horns
    # climbing (then the strings, the brass under them), the piccolo falling in thirty-seconds
    for F, tune, wild in ((F1, SF_DRIVE, False), (F2, SF_DRIVE2, True)):
        s.arp(mo, range(F, F + 8), [4, 2, 3, 1, 2, 0, 1, 2], 0.25, 50, 74, 4, 64 if wild else 60, accent=10,
              gate=0.6)
        s.bass(lo, range(F, F + 8), SF_LOW, 84, lo=36, hi=47)
        s.arp(hp, range(F, F + 8), FALLING_THREES, 0.5, 67 if wild else 62, 86, 3, 58 if wild else 54, accent=8,
              gate=0.5)
        for b in range(F, F + 8):
            if (b - F) % 2 == 0:
                r = root_in(s, b)
                for beat, v in ((0, 80), (1.5, 72), (3, 74)):
                    s.play(tp, b, beat, .5, r, v)
        for blk in (F, F + 4):
            s.crash(blk, 0, 108 if blk == F else (104 if wild else 96))
            for b in (blk, blk + 2):
                for beat in (ACC332 if wild else (0, 1.5)):
                    hit(s, hi_, b, beat, 108, timpani=tp if beat == 0 else None)
        if not wild:
            sung(s, hn, F, tune, 86, e0=80, e1=127, e2=108)
            s.tune(hn2, F, [up(t, -12) for t in tune], 74)
        else:
            sung(s, st, F, [up(t, 12) for t in tune], 82, e0=80, e1=127, e2=108)
            s.tune(st, F, tune, 72)
            s.tune(br, F, [up(t, -12) for t in tune], 80)
            sung(s, hn, F, SF_DRIVE2_HORNS, 70)
        e_down = scale_run(76, 95, HARMONIC)[::-1]           # E harmonic minor, from D sharp down
        s.run(pc, F + 3, 2, e_down[:14], 0.125, 64, -1)
        s.run(pc, F + 7, 2, e_down[2:16], 0.125, 66, -1)
        s.run(st if not wild else hp, F + 5, 3, scale_run(64, 76, HARMONIC)[::-1][:8], 0.125, 52, 2)
    # the riser on B: the spiccato pulsing, the horns swelling, the timpani; the chimes toll B in the last bars
    pulses(s, mo, range(R, n), 56)
    s.ramp(mo, R, 4, 50, 127)
    s.bass(lo, range(R, n), [(0, 1.8, 'r'), (2, 1.8, 'r')], 82, lo=35, hi=46)
    held(s, hn, R, 0, 16, 'F#4', 72, e0=40, e1=127, e2=127)
    held(s, hn2, R, 0, 16, 'D#4', 68, e0=40, e1=127, e2=127)
    s.roll(tp, R, 0, 16, 'B1', 6, 100)
    s.play(cm, R + 2, 0, 2.0, ['B4', 'B3'], 64, 3)
    s.play(cm, R + 3, 0, 2.0, ['B4', 'F#5', 'B3'], 72, 3)
    sweep(s, hp, R + 3, {11, 3, 6}, 47, 87, 58)
    s.end(n)
    return s


# --- ROCKS2: "Drift Field" (D dorian, 118): asteroid and mine fields -----------------
# Exploring among the rocks, the march gone: the spiccato drifts in sixteenths, waves climbing the chord for a bar
# and sinking the next, the low strings strike the 3+3+2, and the horn's tune wanders up D dorian (its B over G,
# its E over D minor), curious, in long notes.  Rocks go by: a string whoosh (the spiccato up and straight back
# down in a breath, fading in from nothing, swelling and fading out) crossing from one side to the other, a harp
# glissando the other way, a timpani rumble rolling past with a lower whoosh over it, a soft tom where each passes
# closest, now and then a near one with the crash and a hit.  The field thickens: B flat lydian and then G flat lydian, a major third lower, the strings' long line with
# the harp in sixteenths, the rocks closer and quicker; then the tune on the strings in octaves with a horn, the
# synth brass's descant over it, hits every other bar, and a riser on A with the rocks crossing both ways.  The
# second pass: the tune on the horns in octaves, the line on the strings in octaves with the horn under it, other
# rocks in other places.

DF_E = ['Dm9', 'G/D', 'Dm9', 'Cmaj7/D', 'Bbmaj7', 'Gm9', 'Fmaj7', 'Asus4 A']
DF_F = ['Bbmaj7#11', 'C/Bb', 'Am7', 'Dm', 'Gbmaj7#11', 'Ab/Gb', 'Fm7', 'A7sus4 A']
DF_THEME = ['A3:0:1 D4:1:1 E4:2:2', 'B4:0:3 A4:3:1', 'F4:0:2 E4:2:1 D4:3:1', 'E4:0:2 G4:2:2', 'F4:0:1 A4:1:1 D5:2:2',
            'C5:0:3 Bb4:3:1', 'A4:0:2 C5:2:1 E5:3:1', 'D5:0:2 C#5:2:2']
DF_LINE = ['D5:0:4', 'E5:0:3 D5:3:1', 'C5:0:4', 'A4:0:2 F4:2:2', 'Bb4:0:4', 'C5:0:3 Bb4:3:1', 'Ab4:0:4',
           'E4:0:2 C#5:2:2']
DF_LINE_UNDER = ['F4:0:4', 'G4:0:4', 'E4:0:4', 'D4:0:4', 'Db4:0:4', 'Eb4:0:4', 'C4:0:4', 'D4:0:2 C#4:2:2']
DF_DESCANT = ['D5:0:4', 'D5:0:4', 'A5:0:4', 'G5:0:4', 'F5:0:4', 'G5:0:2 F5:2:2', 'E5:0:4', 'A5:0:2 E5:2:2']
DF_WAVE = [0, 1, 2, 3, 1, 2, 3, 4, 2, 3, 4, 5, 3, 4, 5, 6, 6, 5, 4, 3, 5, 4, 3, 2, 4, 3, 2, 1, 3, 2, 1, 2]
# the rocks going by, (bar in the pass, beat, beats, what, rightward); the two passes' differ.  (The user,
# 2026-11-17: "could you make a variation on this where it was more of a swooshing sound when they pass by", and
# at 1:02, "the whoosh that happened just before this was pretty cool": the second pass's whoosh at its bar 2,
# two beats up and back down a five-note chord and across from right to left, a soft tom at its closest, in a
# bare bar.  So every whoosh is that one now: two beats, five notes to the octave or more, and fading in from
# nothing and out again; the rumbles roll under one ('roll'), but the second pass's last, after the glint the
# user liked at 1:22 ("the faint sound was nice too"), which stays as it was with its neighbours)
DF_ROCKS = (
    [(1, 2, 2, 'whoosh', True), (4, 1, 1.5, 'glint', False), (6, 2, 2, 'whoosh', False),
     (8, 2, 2, 'whoosh', True), (9, 0.5, 3, 'roll', False), (10, 2, 1.5, 'glint', False),
     (11, 1.5, 2, 'whoosh', True), (12, 0, 2, 'near', False), (13, 2, 1.5, 'glint', True), (14, 0.5, 3, 'roll', True),
     (15, 2, 2, 'whoosh', False), (17, 2, 2, 'whoosh', False), (19, 1, 1.5, 'glint', True),
     (21, 2, 2, 'whoosh', True), (24, 1, 2, 'whoosh', True), (25, 0, 2, 'whoosh', False)],
    [(0, 2.5, 1.5, 'glint', True), (2, 1, 2, 'whoosh', False), (5, 2, 2, 'whoosh', True),
     (7, 0.5, 1.5, 'glint', False), (8, 0.5, 3, 'roll', True), (9, 2, 2, 'whoosh', False),
     (10, 0, 2, 'near', True), (11, 2, 1.5, 'glint', False), (12, 1.5, 2, 'whoosh', True),
     (13, 0.5, 3, 'rumble', False),
     (14, 2, 2, 'whoosh', True), (15, 1, 1.5, 'glint', False), (16, 2, 2, 'whoosh', True),
     (18, 2, 1.5, 'glint', False), (20, 0, 2, 'near', False), (22, 2, 2, 'whoosh', True),
     (24, 1, 2, 'whoosh', False), (25, 0, 2, 'whoosh', True)])


def drift16(s, ch, bars, vel, lo=50):
    """the spiccato drifting: sixteenths up the chord's ladder in overlapping fours for a bar, down the next"""
    for b in bars:
        for k in range(16):
            beat = k * 0.25
            c = chord(s.sym_at(b, beat))
            lad = ladder(c, in_range(c.bass, lo, lo + 11))
            s.play(ch, b, beat, 0.15, lad[DF_WAVE[(b % 2) * 16 + k]], vel + (10 if beat in ACC332 else 0)
                   - (6 if k % 2 else 0), 3)


def cross(s, ch, bar, beat, notes, beats, vel, rightward, lo_vel=0.3, hold=1.3, fade=None):
    """notes evenly over these beats, swelling to the middle and fading, panned from one side to the other;
    fade: the channel's expression at the ends too (rising to full at the middle and falling back to it), so
    the notes still ringing swell and fade with them"""
    sides = (PAN_L, PAN_C, PAN_R) if rightward else (PAN_R, PAN_C, PAN_L)
    step, last, last_e = beats / len(notes), None, None
    for k, q in enumerate(notes):
        x = k / max(1, len(notes) - 1)
        b, bt = bar_beat(s, bar * s.beats + beat + k * step)
        side = sides[min(2, int(x * 3))]
        if side != last:
            s.cc(ch, b, bt, 10, side)
            last = side
        if fade is not None:
            e = round(fade + (127 - fade) * (1 - abs(2 * x - 1)))
            if e != last_e:
                s.cc(ch, b, bt, 11, e)
                last_e = e
        v = vel * (lo_vel + (1 - lo_vel) * (1 - abs(2 * x - 1)))
        s.play(ch, b, bt, min(step * hold, 0.3), q, max(8, round(v)), 2)


def chord_pcs(s, bar, beat, ninth=True):
    c = chord(s.sym_at(bar, beat))
    return {(c.root + t) % 12 for t in c.tones} | ({(c.root + 2) % 12} if ninth else set())


def swoosh_pcs(s, bar, beat):
    """the chord and its ninth, and its sixth too where that leaves under five notes to the octave (a triad's
    whoosh was a coarse arpeggio; the one the user liked had five), unless the sixth rubs a half step on one"""
    pcs = chord_pcs(s, bar, beat)
    if len(pcs) < 5:
        six = (chord(s.sym_at(bar, beat)).root + 9) % 12
        if (six + 1) % 12 not in pcs and (six - 1) % 12 not in pcs:
            pcs.add(six)
    return pcs


def rock(s, chans_, bar, beat, beats, what, rightward):
    """a rock going by: a whoosh (the spiccato up and back, fading in from nothing and out again), a glint (the
    harp's glissando), a rumble (the timpani rolling past), a roll (a whoosh, lower, over a rumble), or a near
    one (a whoosh over a rumble, the crash and a hit at its closest)"""
    sw, gl, rb, hi_ = chans_
    if what in ('whoosh', 'near', 'roll'):
        lo = 50 if what == 'roll' else 55                   # (a rolling rock's lower: bigger)
        rise = [q for q in range(lo, lo + 37) if q % 12 in swoosh_pcs(s, bar, beat)]
        mid = beat + beats / 2 - 1 if what == 'roll' else beat     # (two beats about its closest)
        cross(s, sw, bar, mid, rise + rise[-2::-1], 2 if what == 'roll' else beats, 124 if what == 'near' else 114,
              rightward, 0.35, 2.0, fade=84)
    if what == 'glint':
        rise = [q for q in range(67, 96) if q % 12 in chord_pcs(s, bar, beat, ninth=False)]
        cross(s, gl, bar, beat, rise + rise[-2:-5:-1], beats, 84, rightward, lo_vel=0.5)
    if what in ('rumble', 'near', 'roll'):
        q = in_range(chord(s.sym_at(bar, beat)).root, 33, 44)
        cross(s, rb, bar, beat, [q] * int(beats * 8), beats, 62 if what == 'near' else 56, rightward, lo_vel=0.1,
              hold=0.6 if what == 'roll' else 1.3)
    b, bt = bar_beat(s, bar * s.beats + beat + beats / 2)  # where it passes closest
    if what == 'near':
        s.crash(b, bt, 74)
        hit(s, hi_, b, bt, 92)
    elif what != 'glint':
        s.play(9, b, bt, 0.2, DRUM['tom3'] if what == 'whoosh' else DRUM['tom4'], 38 if what == 'whoosh' else 46, 3)


def drift_field():
    pass_ = DF_E + DF_F + DF_E + ['Asus4', 'A']    # 8 + 8 + 8 + 2 = 26 bars
    form = ['Dm9', 'Dm9'] + pass_ + pass_
    s = Piece(118, 3313, form, level=2.0)
    hn, hn2, mo, lo, hi_, br, pc, tp, hp, st, bs = chans(s)
    sw, gl, rb = 12, 13, 14                         # the rocks: the spiccato, the harp and the timpani, panned
    s.setup(sw, 44, ORCH['motor'][2])
    s.setup(gl, 46, ORCH['harp'][2])
    s.setup(rb, 47, ORCH['timpani'][2])
    rocks = (sw, gl, rb, hi_)
    n = len(form)                                   # 2 + 26 + 26 = 54 bars, 110 s
    s.loop_at(2)
    drift16(s, mo, range(0, 2), 52)                 # the intro: the drift coming up, a rock far off
    s.ramp(mo, 0, 2, 50, 120)
    pedal332(s, lo, range(1, 2), 76)
    rock(s, rocks, 1, 1, 2, 'whoosh', True)
    for P, second in ((2, False), (28, True)):
        E1, F, E2, R = P, P + 8, P + 16, P + 24
        for b in (E1, E1 + 4, F, F + 4, E2, E2 + 4):
            s.sect(b)
        drift16(s, mo, range(E1, F), 56)
        drift16(s, mo, range(E2, R + 2), 62)
        pedal332(s, lo, range(E1, F), 84)
        pedal332(s, lo, range(E2, R + 2), 88)
        # the tune, wandering up D dorian (the horns in octaves the second time)
        s.play(tp, E1, 0, 1, 'D2', 80)
        hit(s, hi_, E1, 0, 98)
        sung(s, hn, E1, DF_THEME, 84, e0=70, e1=122, e2=96)
        if second:
            s.tune(hn2, E1, [up(t, -12) for t in DF_THEME], 70)
            s.crash(E1, 0, 98)
        s.run(pc, E1 + 3, 2, scale_up(74, 2, (0, 2, 3, 5, 7, 9, 10), 8), 0.25, 54, 2)   # D dorian up
        # the field thickens: B flat, then G flat, lydian; the harp's sixteenths, the strings' line
        s.arp(hp, range(F, F + 8), [0, 2, 1, 3], 0.25, 58, 82, 4, 46, accent=6)
        s.arp(mo, range(F, F + 8), [0, 1, 2, 1, 3, 2, 1, 2], 0.5, 48, 66, 3, 58)
        pedal332(s, lo, range(F, F + 8), 82)
        s.crash(F, 0, 96)
        s.roll(tp, F + 3, 2, 2, 'A1', 20, 80, step=0.125)
        if not second:
            sung(s, st, F, DF_LINE, 78, e0=56, e1=118, e2=86)
            sung(s, hn, F + 4, DF_LINE_UNDER[4:], 62)
        else:
            sung(s, st, F, [up(t, 12) for t in DF_LINE], 76, e0=56, e1=118, e2=86)
            s.tune(st, F, DF_LINE, 68)
            sung(s, hn, F, DF_LINE_UNDER, 64)
        s.run(pc, F + 3, 2, scale_up(81, 2, MINOR, 8), 0.25, 56, 2)             # D minor up, into G flat
        # the tune on the strings with the synth brass's descant; hits
        s.crash(E2, 0, 104)
        for b in (E2, E2 + 4):
            hit(s, hi_, b, 0, 106, timpani=tp)
        for b in (E2 + 2, E2 + 6):
            hit(s, hi_, b, 0, 100 if second else 96)
        s.bass(bs, range(E2, R), [(0, 3.9, 'r')], 64, lo=29, hi=41)
        sung(s, st, E2, [up(t, 12) for t in DF_THEME], 84, e0=76, e1=127, e2=104)
        s.tune(st, E2, DF_THEME, 76)
        sung(s, hn2 if second else hn, E2, DF_THEME, 76, e0=70, e1=118, e2=92)
        sung(s, br, E2, DF_DESCANT, 78, e0=74, e1=122, e2=98)
        # the riser on A: the horns swelling, the timpani rolling, rocks crossing both ways
        held(s, hn, R, 0, 8, 'E4', 72, e0=40, e1=127, e2=127)
        held(s, hn2, R, 0, 8, 'A3', 66, e0=40, e1=127, e2=127)
        s.roll(tp, R + 1, 0, 4, 'A1', 8, 50 if second else 44)
        for bar, beat, beats, what, rightward in DF_ROCKS[1 if second else 0]:
            rock(s, rocks, P + bar, beat, beats, what, rightward)
    s.end(n)
    return s


# --- LAUNCH2: "Catapult" (C minor into E, 156): the flight deck and the launch --------
# The deck: eighths pulsing on C in octaves, the low strings striking the 3+3+2 over the pedal, the timpani's
# heartbeat, and the horn's call a fifth up and held (C to G: the thrust's tune in the minor, before it is known),
# the second horn an octave under it and the brass's low line in the second phrase, the harp breaking in; G7 over
# B.  The catapult:
# the bass stays on B, and everything winds up, the spiccato's B's twice as fast and twice again, a run up three
# octaves, the piccolo and the harp after it, the timpani rolling, the horns swelling on B7, and on the downbeat
# the crash and the hit: E major, a third away, the thrust.  Outbound's motor in octaves, the hits on the 3+3+2,
# the synth brass's fanfare (E up to B, the same fifth) over chords moving by thirds (E, C, A, B; E, G, C, B), the
# horn's line under it.  Then lifted a major third into A flat for the strings' broad tune, home through A flat and
# B flat to G, the horns swelling on G over the timpani into the deck again.

CP_DECK = ['Cm', 'Ab/C', 'Fm/C', 'Cm', 'Cm', 'Db/C', 'Ab/C', 'G7/B']
CP_SHOT = ['B', 'B7']
CP_THRUST = ['E', 'Cmaj7', 'A', 'B', 'E', 'G', 'C', 'Bsus4 B']
CP_LIFT = ['Ab', 'Fm9', 'Dbmaj7', 'Eb']
CP_HOME = ['Ab', 'Bb', 'G', 'G']
CP_CALL = ['C4:0:1 G4:1:3', 'Ab4:0:4', 'F4:0:1 C5:1:3', 'G4:0:4', 'C4:0:1 G4:1:3', 'Ab4:0:2 F4:2:2',
           'Eb4:0:3 C4:3:1', 'D4:0:2 F4:2:2']
CP_CALL_UNDER = ['G3:0:4', 'F3:0:4', 'Eb3:0:4', 'D3:0:2 B2:2:2']
CP_FANFARE = ['E4:0:1 B4:1:3', 'C5:0:2 B4:2:1 G4:3:1', 'A4:0:1 C#5:1:1 E5:2:2', 'F#5:0:3 D#5:3:1',
              'B4:0:1 E5:1:3', 'D5:0:3 G5:3:1', 'E5:0:2 G5:2:2', 'E5:0:2 D#5:2:2']
CP_HORNS = ['G#4:0:4', 'G4:0:4', 'E4:0:4', 'F#4:0:4', 'G#4:0:4', 'G4:0:4', 'G4:0:4', 'F#4:0:4']
CP_LIFT_TUNE = ['C5:0:3 Eb5:3:1', 'Ab5:0:2 G5:2:1 F5:3:1', 'F5:0:3 Ab5:3:1', 'G5:0:4']
CP_LIFT_UNDER = ['Eb4:0:4', 'C4:0:4', 'C4:0:4', 'Bb3:0:4']
CP_HOME_TUNE = ['Eb5:0:3 C5:3:1', 'D5:0:3 F5:3:1']
CP_HOME_UNDER = ['C4:0:4', 'Bb3:0:4']


def catapult():
    pass_ = CP_DECK + CP_SHOT + CP_THRUST + CP_LIFT + CP_HOME    # 8 + 2 + 8 + 4 + 4 = 26 bars
    form = ['Cm', 'Cm'] + pass_
    s = Piece(156, 4156, form, level=-1.6)
    hn, hn2, mo, lo, hi_, br, pc, tp, hp, st, bs = chans(s)
    n = len(form)                                   # 2 + 26 = 28 bars, 43 s
    DK, SH, TH, LF, HM = 2, 10, 12, 20, 24
    R = HM + 2
    s.loop_at(DK)
    for b in (DK, DK + 4, SH, TH, TH + 4, LF, HM):
        s.sect(b)
    # the intro: the deck's pulse coming up, the timpani rolling into the call
    pulses(s, mo, range(0, 2), 52)
    s.ramp(mo, 0, 2, 40, 100)
    s.play(lo, 0, 0, 7.8, 'C2', 70)
    s.roll(tp, 1, 0, 4, 'C2', 10, 80)
    # the deck: the pulse on C growing, the low strings, the timpani's heartbeat, the horn's call
    pulses(s, mo, range(DK, SH), 54)
    s.ramp(mo, DK, 8, 80, 127)
    pedal332(s, lo, range(DK, SH), 86)
    for b in range(DK, SH):
        s.play(tp, b, 0, 0.5, 'C2', 74 + (b - DK) * 2)
        s.play(tp, b, 0.5, 0.5, 'C2', 58 + (b - DK) * 2)
    s.crash(DK, 0, 96)
    hit(s, hi_, DK, 0, 100)
    hit(s, hi_, DK + 4, 0, 104)
    sung(s, hn, DK, CP_CALL, 82, e0=66, e1=124, e2=96)
    s.tune(hn2, DK + 4, [up(t, -12) for t in CP_CALL[4:]], 68)
    sung(s, br, DK + 4, CP_CALL_UNDER, 60, e0=50, e1=110, e2=90)
    s.arp(hp, range(DK + 4, SH), [0, 1, 2, 3, 4, 3, 2, 1], 0.5, 55, 79, 4, 44, gate=0.6)
    s.run(pc, DK + 3, 2, scale_up(79, 0, MINOR, 8), 0.25, 54, 2)            # G up, C minor
    # the catapult: B in the bass, the B's twice as fast and twice again, a run up three octaves, the shot
    for k in range(4):
        s.play(mo, SH, k * 0.5, 0.4, [47, 59], 70 + 3 * k, 3)
    for k in range(8):
        s.play(mo, SH, 2 + k * 0.25, 0.22, [47, 59], 80 + 2 * k, 3)
    s.run(mo, SH + 1, 0, scale_up(47, 11, MIXOLYDIAN, 22), 3 / 22, 68, 1)   # B up three octaves and more
    s.play(bs, SH, 0, 7.8, 'B1', 84)
    for beat in range(4):
        s.play(lo, SH, beat, 0.8, 'B1', 88 + beat * 2)
    s.play(lo, SH + 1, 0, 3.8, 'B1', 96)
    held(s, hn, SH, 0, 8, 'F#4', 74, e0=40, e1=127, e2=127)
    held(s, hn2, SH, 0, 8, 'D#4', 70, e0=40, e1=127, e2=127)
    s.pad(br, range(SH + 1, TH), 57, 71, 3, 66)
    s.roll(tp, SH, 0, 8, 'B1', 8, 100)
    s.run(pc, SH + 1, 2, scale_up(83, 11, MIXOLYDIAN, 8), 0.25, 62, 2)      # B up to B, into the downbeat
    sweep(s, hp, SH + 1, {11, 3, 6, 9}, 47, 90, 60, beats=2)
    # the thrust: E, Outbound's motor in octaves, the hits on the 3+3+2, the fanfare, the horns' thirds
    octaves(s, mo, range(TH, LF), 68)
    pedal332(s, lo, range(TH, LF), 96)
    s.bass(bs, range(TH, LF), [(0, 3.9, 'r')], 80, lo=28, hi=40)
    for blk in (TH, TH + 4):
        s.crash(blk, 0, 116 if blk == TH else 110)
        for b in (blk, blk + 2):
            for beat in ACC332:
                hit(s, hi_, b, beat, 114, timpani=tp if beat == 0 else None)
    s.tune(br, TH, CP_FANFARE, 90)
    sung(s, hn, TH, CP_HORNS, 76)
    s.run(pc, TH + 3, 2, scale_up(83, 4, MAJOR, 8), 0.25, 60, 2)             # E major from B up
    s.tune(pc, TH + 6, [up(t, 12) for t in CP_FANFARE[6:]], 54)
    # lifted: A flat, the strings' broad tune, the horns under; home through A flat and B flat to G
    s.arp(mo, range(LF, R), [0, 2, 1, 3, 2, 1, 3, 2], 0.5, 52, 76, 4, 62)
    pedal332(s, lo, range(LF, R), 92)
    s.crash(LF, 0, 110)
    for b in (LF, LF + 2, HM):
        for beat in (0, 1.5):
            hit(s, hi_, b, beat, 108, timpani=tp if beat == 0 else None)
    sung(s, st, LF, [up(t, 12) for t in CP_LIFT_TUNE] + [up(t, 12) for t in CP_HOME_TUNE], 78, e0=70, e1=124, e2=100)
    s.tune(st, LF, CP_LIFT_TUNE + CP_HOME_TUNE, 70)
    sung(s, hn, LF, CP_LIFT_UNDER + CP_HOME_UNDER, 70)
    s.run(pc, LF + 3, 2, scale_up(79, 3, MAJOR, 8), 0.25, 60, 2)             # E flat from G up
    # the riser on G: the motor in octaves, the horns swelling, the timpani rolling up, the harp
    octaves(s, mo, range(R, n), 66)
    pedal332(s, lo, range(R, n), 92)
    held(s, hn, R, 0, 8, 'D4', 76, e0=40, e1=127, e2=127)
    held(s, hn2, R, 0, 8, 'B3', 70, e0=40, e1=127, e2=127)
    s.roll(tp, R, 0, 8, 'G1', 6, 110)
    sweep(s, hp, R + 1, {7, 11, 2}, 43, 86, 58)
    s.end(n)
    return s


SONGS = {'CRUISE9.MID': apogee, 'CRUISE10.MID': faint_signal, 'CRUISE11.MID': nightside, 'CRUISE12.MID': starfall,
         'ROCKS2.MID': drift_field, 'LAUNCH2.MID': catapult}
