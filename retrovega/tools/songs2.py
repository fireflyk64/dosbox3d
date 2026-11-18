"""The second room songs, for our OPL3 bank (tools/oplbank.py); CC0, ours.  Each is the other song its room may
pick (the user, 2026-11-17, in the soundtrack review: of Off-Watch, "another song with a similar theme but some
differences"; of Bunk Light, "another song like this which can be randomly selected when going into that room...
perhaps a little shorter and way more relaxing even").

    BAR2.MID       "Dog Watch": the bar again.  Warm, moving, still easy: 104 a minute, swung, some eight and a
                   half minutes round; an upright bass where Off-Watch has the fingered one, and its own tunes:
                   I   Dog Watch (D minor): the head on jazz guitar and vibraphone in unison over a falling line
                       (D, C sharp, C, B under the minor chord), electric-piano comping, the walking bass, ride
                       and a cross-stick on four; the tine piano takes a chorus, the guitar the bridge;
                   II  Harbour Lights (A flat): a ballad in the same tempo, the band in two: the clarinet low and
                       warm over held piano chords and soft strings, the guitar on the bridge;
                   III Salt Road (C minor, latin, straight eighths): the piano's montuno, the bass anticipating
                       the bar, rim on the clave and a shaker, coming in over eight bars under the clarinet, which
                       carries the ballad's tune on into the latin time; the guitar's tune in octaves from the
                       clarinet's last note, a vibraphone chorus over the organ, the tune again;
                   IV  Galley Blues (G, a jazz blues): an organ trio, the organ comping in place of the piano;
                       the guitar's head, then with the vibraphone, a tine-piano chorus, the head out;
                   V   Dog Watch again (D minor): the head on flute and guitar, the guitar's own bridge, the
                       last eight on guitar and vibraphone as at the start, turning round into the top.
    QUARTER2.MID   "Quiet Hours": the quarters again.  Slower and far quieter than Bunk Light: 54 a minute, no
                   drums, some four and three quarter minutes round:
                   I   Quiet Hours (D flat): a soft piano rolling its chords once a bar under a wide pad, the
                       upright on the downbeat, the tine piano's tune in long notes;
                   II  Drift (B flat minor): running water on the harp (eighths rippling on each chord's
                       pentatonic) under the soft pad, the vibraphone's long notes above it;
                   III Lantern (G flat): nylon guitar picked in quarters, a low flute, two far celesta notes;
                   IV  Quiet Hours again (D flat): the tune on the vibraphone, then the piano breaking its chords
                       a note a beat, the tune's second half on the tine piano with an E flat for colour and the
                       vibraphone's long notes falling by half steps above it, into the first bar.

Each loops (its last bar leads back into its first) and carries a "sect" marker at each later section's start,
the places the room's song may begin (src/game.c room_music).  The heads of Dog Watch and Harbour Lights and the
tune of Quiet Hours are written out note by note ('PITCH:BEAT:LENGTH' a bar); the rest comes from
tools/compose.py's Tune, seeded, as in tools/songs.py.
"""
import contextlib
import random

from compose import chord, bars_of, Voicer, Bass, Tune, SWING, SWING_END, SOLO, SOLO_END
from songs import Comp, put, shift, comp, swing_drums
from orch import in_time


def transpose(notes, semis):
    return [(b, beat, dur, q + semis) for b, beat, dur, q in notes]


def unplayed(s, n=1, spread=4):
    """n notes left out, their velocity draws made all the same (Comp.play draws one a pitch): every note after
    them plays as it did"""
    for _ in range(n):
        s.rng.randint(-spread, spread)


@contextlib.contextmanager
def own_draws(s, seed):
    """notes added with their own velocity draws, so none of the song's other notes change"""
    keep, s.rng = s.rng, random.Random(seed)
    try:
        yield
    finally:
        s.rng = keep


# --- the bass lines: the chord's tones from its own root (a slash chord's bass first), a step into the next ---

def tones_of(c):
    """the chord's root, third, fifth and seventh as pitch classes (no extensions; a fifth added if it has none)"""
    ts = [t for t in c.tones if t < 12]
    if not any(t in (6, 7, 8) for t in ts):
        ts.append(7)
    return {(c.root + t) % 12 for t in ts}


def near(pcs, q, up):
    """the next pitch above (or below) q whose class is in pcs"""
    steps = range(q + 1, q + 13) if up else range(q - 1, q - 13, -1)
    return next(x for x in steps if x % 12 in pcs)


def into(pc, prev, below, bass):
    """a half step into pitch class pc, from below or above, aimed at its octave nearest prev in the bass's range;
    the bass is told, so the next bar's note is the one aimed at"""
    t = min((x for x in range(bass.lo, bass.hi + 1) if x % 12 == pc), key=lambda x: abs(x - prev))
    bass.last = t
    return t - 1 if below and t > 28 else t + 1


def fifth_of(c, q, lo=28, hi=47):
    """the chord's fifth (flat in a half-diminished or diminished chord) nearest q, above it where it fits"""
    pc = (c.root + (6 if 6 in c.tones and 7 not in c.tones else 7)) % 12
    cands = [x for x in range(lo, hi + 1) if x % 12 == pc]
    return min([x for x in cands if x > q] or cands, key=lambda x: abs(x - q))


def fit(q, lo=28, hi=50):
    while q > hi:
        q -= 12
    while q < lo:
        q += 12
    return q


def walk2(s, ch, bar, syms, nxt, bass, vel=80):
    """a walking line: the bass note, chord tones up (or down from high), a half step into the next bar's bass"""
    cs = [chord(x) for x in syms]
    if len(cs) == 1:
        c = cs[0]
        b = bass.note(c)
        up = b < (40 if bar % 2 == 0 else 35)
        x = near(tones_of(c), b, up)
        line = [b, x, near(tones_of(c), x, up)]
    else:
        b1 = bass.note(cs[0])
        b2 = bass.note(cs[1])
        between = [q for q in range(min(b1, b2) + 1, max(b1, b2)) if q % 12 in tones_of(cs[0])]
        x = between[len(between) // 2] if between else (b2 - 1 if b1 < b2 else b2 + 1)
        line = [b1, x, b2]
    line.append(into(chord(nxt).bass, line[-1], bar % 3 != 1, bass))
    for beat, q in enumerate(line):
        s.play(ch, bar, beat, 0.9, fit(q), vel if beat == 0 else vel - 9)
    if bar % 4 == 3:
        s.play(ch, bar, 2.5, 0.4, fit(line[2] + 12 if line[2] < 40 else line[2] - 12), vel - 20)   # a skip in the turn


def two_feel(s, ch, bar, syms, nxt, bass, vel=78):
    """the ballad's bass: half notes, the bass and its fifth, or a step into the next bar"""
    cs = [chord(x) for x in syms]
    if len(cs) == 2:
        s.play(ch, bar, 0, 1.9, bass.note(cs[0]), vel)
        s.play(ch, bar, 2, 1.9, bass.note(cs[1]), vel - 6)
        return
    b = bass.note(cs[0])
    second = fifth_of(cs[0], b) if bar % 2 == 0 else into(chord(nxt).bass, b, bar % 4 == 1, bass)
    s.play(ch, bar, 0, 1.9, b, vel)
    s.play(ch, bar, 2, 1.9, fit(second), vel - 8)


def tumbao(s, ch, bar, syms, nxt, bass, vel=78, first=False, last=False):
    """the latin bass: the fifth on the and of two, the next bar's bass anticipated on four and held over the
    bar line (so the downbeat is the last bar's); a section's first bar plays its downbeat, its last does not
    anticipate"""
    cs = [chord(x) for x in syms]
    b = bass.note(cs[0])
    if first:
        s.play(ch, bar, 0, 1.4, b, vel)
    if len(cs) == 2:
        s.play(ch, bar, 1.5, 0.45, fifth_of(cs[0], b), vel - 10)
        s.play(ch, bar, 2, 0.95, bass.note(cs[1]), vel - 4)
    else:
        s.play(ch, bar, 1.5, 1.4, fifth_of(cs[0], b), vel - 8)
    if last:
        s.play(ch, bar, 3, 0.9, bass.last, vel - 6)
    else:
        q = min((x for x in range(28, 46) if x % 12 == chord(nxt).bass), key=lambda x: abs(x - bass.last))
        bass.last = q
        s.play(ch, bar, 3, 1.45, q, vel)


# --- BAR2: "Dog Watch" ---

DW_A1 = bars_of('Dm9 | Dmmaj7/C# | Dm7/C | Bm7b5 | Gm9 | C13 | Fmaj9 | Em7b5 A7b9')
DW_A2 = DW_A1[:6] + bars_of('Fmaj9 | Cm9 F13')
DW_B = bars_of('Bbmaj9 | Eb13 | Am7 | D7b9 | Gm9 | C13 | Fmaj9 | Em7b5 A7b9')
DW_A3 = DW_A1[:6] + bars_of('Bbmaj7#11 | Em7b5 A7b9')
DW_OUT = DW_A1[:6] + bars_of('Bbmaj7#11 | Bbm9 Eb13')         # the last eight turning to A flat (II)
DOG_WATCH = DW_A1 + DW_A2 + DW_B + DW_A3 + DW_A1 + DW_A2 + DW_B + DW_OUT
DW_HEAD_A = ['F4:0:.5 A4:.5:.5 E5:1:2 D5:3:.5 C5:3.5:.5',
             'C#5:0:2 A4:2:1 E4:3:1',
             'F4:0:.5 A4:.5:.5 D5:1:2 C5:3:.5 B4:3.5:.5',
             'A4:0:2.5 F4:3:.5 A4:3.5:.5',
             'Bb4:0:.5 D5:.5:.5 F5:1:2 E5:3:.5 D5:3.5:.5',
             'E5:0:1.5 D5:1.5:.5 Bb4:2:1 G4:3:1',
             'A4:0:.5 C5:.5:.5 G5:1:2.5 F5:3.5:.5',
             'E5:0:1 D5:1:.5 Bb4:1.5:.5 C#5:2:2']
DW_HEAD_A2 = DW_HEAD_A[:6] + ['A4:0:1.5 C5:1.5:.5 E5:2:2', 'Eb5:0:1.5 D5:1.5:.5 C5:2:1 A4:3:1']
DW_HEAD_B = ['D5:0:2.5 C5:2.5:.5 D5:3:.5 F5:3.5:.5',
             'G5:0:2 F5:2:.5 Db5:2.5:1.5',
             'C5:0:2.5 B4:2.5:.5 C5:3:.5 E5:3.5:.5',
             'F#5:0:2 Eb5:2:.5 C5:2.5:1.5',
             'Bb4:0:2.5 A4:2.5:.5 Bb4:3:.5 D5:3.5:.5',
             'E5:0:2 D5:2:.5 Bb4:2.5:1.5',
             'A4:0:1 C5:1:1 E5:2:1 G5:3:1',
             'G5:0:.5 F5:.5:.5 E5:1:.5 D5:1.5:.5 C#5:2:1 A4:3:1']
DW_HEAD_A3 = DW_HEAD_A[:6] + ['A4:0:.5 D5:.5:.5 F5:1:.5 E5:1.5:2.5', 'D5:0:1 Bb4:1:1 A4:2:1.5 E4:3.5:.5']
DW_HEAD_OUT = DW_HEAD_A3[:7] + ['Db5:0:1 F4:1:1 G4:2:2']

HB_A1 = bars_of('Abmaj9 | Fm9 | Dbmaj9 | Gm7b5 C7b9 | Fm9 | Bb13 | Bbm9 | Eb9sus4')
HB_A2 = HB_A1[:6] + bars_of('Bbm9 Eb13 | Abmaj9')
HB_B = bars_of('Ebm9 | Ab13 | Dbmaj9 | Dbm6 | Cm7 | F7b9 | Bbm9 | Eb9sus4')
HB_A3 = HB_A1[:6] + bars_of('Bbm9 Eb13 | Dm7b5 G7b9')         # turning to C minor (III)
HARBOUR = HB_A1 + HB_A2 + HB_B + HB_A3
HB_TUNE_A = ['Eb4:0:1 Ab4:1:1 C5:2:2', 'Bb4:0:3 G4:3:1', 'Ab4:0:1 C5:1:1 Eb5:2:2', 'Db5:0:1.5 C5:1.5:.5 Bb4:2:1 E4:3:1',
             'F4:0:1 G4:1:1 Ab4:2:1 C5:3:1', 'D5:0:3 C5:3:1', 'Db5:0:2 C5:2:1 Bb4:3:1', 'Ab4:0:3']
HB_TUNE_A2 = HB_TUNE_A[:6] + ['Db5:0:1.5 C5:1.5:.5 Bb4:2:1 G4:3:1', 'Ab4:0:3']
HB_TUNE_B = ['Gb4:0:1 Bb4:1:1 Db5:2:2', 'C5:0:3 Bb4:3:1', 'F4:0:1 Ab4:1:1 C5:2:2', 'Bb4:0:3 Ab4:3:1',
             'Eb5:0:1.5 D5:1.5:.5 C5:2:1 Bb4:3:1', 'C5:0:2 A4:2:1 Gb4:3:1', 'F4:0:2 Db5:2:2',
             'Db5:0:1.5 Bb4:1.5:.5 Ab4:2:2']
HB_TUNE_A3 = HB_TUNE_A[:6] + ['Db5:0:1.5 C5:1.5:.5 Bb4:2:1 G4:3:1', 'Ab4:0:2 B4:2:2']

SR_A = bars_of('Cm9 | Abmaj7#11 | Fm9 | Dm7b5 G7b9 | Cm9 | Bbm9 Eb13 | Abmaj9 | Dm7b5 G7b9')
SR_B = bars_of('Fm9 | Bb13 | Ebmaj9 | Abmaj9 | Dm7b5 | G7b9 | Cm9 | Dm7b5 G7b9')
SALT_ROAD = (SR_A + SR_B) * 2 + SR_A + SR_B[:7] + bars_of('Am7b5 D7b9')   # the last bar turning to G (IV)
# the clarinet's way in: Harbour Lights' first eight bars (HB_TUNE_A) over Salt Road's chords, the half notes
# pushed onto the off-beats; the last bar falls to the guitar's first note
SR_WAY_IN = ['Eb4:0:1 G4:1:1.5 C5:2.5:1.5', 'Bb4:0:2.5 G4:2.5:1.5', 'Ab4:0:1 C5:1:1.5 Eb5:2.5:1.5',
             'D5:0:1.5 C5:1.5:.5 Ab4:2:1 B4:3:1', 'C5:0:1 G4:1:1.5 D5:2.5:1.5', 'Db5:0:2 C5:2:2',
             'C5:0:1.5 Bb4:1.5:1 G4:2.5:1.5', 'Ab4:0:2 G4:2:2']
# the latin tune's rhythms: long notes pushed ahead of the beat
LATIN = [[(0, 1.5), (1.5, 1), (2.5, 1.5)], [(0, .5), (.5, 1), (1.5, 1), (2.5, 1.5)], [(0, 2), (2.5, .5), (3, 1)],
         [(.5, 1), (1.5, 1), (2.5, 1.5)], [(0, 1), (1, .5), (1.5, 1), (2.5, 1.5)], [(0, 1.5), (1.5, .5), (2, 2)]]
LATIN_END = [[(0, 3)], [(0, 1.5), (1.5, 2)], [(0, 3.5)]]

GB = bars_of('G13 | C9 | G13 | Dm9 G13 | C9 | C#dim7 | Bm7 | E7b9 | Am9 | D13 | Bm7 E7b9 | Am7 D7b9')
GALLEY = GB * 3 + GB[:11] + bars_of('Em7b5 A7b9')                # the last bar turning to D minor (V)

DW_AGAIN = DW_A1 + DW_A2 + DW_B + DW_A3
BAR2 = [('Dog Watch', DOG_WATCH), ('Harbour Lights', HARBOUR), ('Salt Road', SALT_ROAD), ('Galley Blues', GALLEY),
        ('Dog Watch again', DW_AGAIN)]


def dw_drums(s, bar, fill=False, lift=False):
    """ride ding, ding-da; the foot's hat on two and four; a cross-stick on four only; a soft kick"""
    for beat in (0, 1, 1.5, 2, 3, 3.5):
        s.play(9, bar, beat, 0.2, 51, 54 if beat in (1, 3) else 44, spread=5)
    for beat in (1, 3):
        s.play(9, bar, beat, 0.2, 44, 48)
    s.play(9, bar, 3, 0.2, 37, 64 if lift else 58)
    s.play(9, bar, 0, 0.2, 36, 48)
    if fill:
        s.play(9, bar, 1.5, 0.2, 37, 46)
        s.play(9, bar, 2.5, 0.2, 36, 42)


def ballad_drums(s, bar):
    """the ride on the beats, softly, the foot's hat on two and four, a kick every other bar"""
    for beat in range(4):
        s.play(9, bar, beat, 0.2, 51, 40 if beat in (1, 3) else 34, spread=4)
    for beat in (1, 3):
        s.play(9, bar, beat, 0.2, 44, 40)
    if bar % 2 == 0:
        s.play(9, bar, 0, 0.2, 36, 36)


def latin_drums(s, bar, k, level=1.0, rim=True):
    """rim on the clave (its two side, then its three), the shaker's eighths, a soft kick, the foot's hat; level
    scales it all (Salt Road coming in), and rim=False leaves the clave out"""
    for beat in ((1, 2) if k % 2 == 0 else (0, 1.5, 3)):
        if rim:
            s.play(9, bar, beat, 0.2, 37, round(60 * level))
        else:
            unplayed(s)
    for e in range(8):
        s.play(9, bar, e * 0.5, 0.2, 70, round((36 if e % 2 == 1 else 28) * level), spread=3)
    s.play(9, bar, 0, 0.2, 36, round(44 * level))
    s.play(9, bar, 2.5, 0.2, 36, round(34 * level))
    for beat in (1, 3):
        s.play(9, bar, beat, 0.2, 44, round(30 * level))


def montuno(s, ch, bar, syms, keys, vel, k):
    """the piano's two-bar figure in straight eighths, broken from the chord's voicing (low pair, top, middle)"""
    vs = [keys.next(chord(x)) for x in syms]
    figure = ([(0, (0, 1)), (1, (3,)), (1.5, (2,)), (2.5, (3,)), (3, (0, 1)), (3.5, (2,))] if k % 2 == 0 else
              [(.5, (3,)), (1, (2,)), (2, (0, 1)), (2.5, (3,)), (3.5, (2,))])
    for beat, idx in figure:
        v = vs[1 if beat >= 2 and len(vs) == 2 else 0]
        s.play(ch, bar, beat, 0.45, [v[i] for i in idx], vel + (6 if beat in (0, 1.5, 2.5) else 0))


def bar2_song():
    s = in_time(Comp(104, seed=17, swing=0.64))
    s.setup(0, 5, 86)       # electric piano, the warm one: the comping
    s.setup(1, 32, 106)     # upright bass (Off-Watch walks on the fingered one)
    s.setup(2, 26, 98)      # jazz guitar
    s.setup(3, 11, 68)      # vibraphone, back in the band (the user, of Off-Watch: the bell too far forward)
    s.setup(4, 16, 56)      # organ: Galley Blues' comping, and under the vibraphone's latin chorus
    s.setup(5, 4, 84)       # electric piano with the tine: its choruses
    s.setup(7, 73, 86)      # flute
    s.setup(8, 71, 92)      # clarinet: the ballad
    s.setup(9, 0, 96)       # drums
    s.setup(10, 48, 64)     # strings, soft, under the ballad
    whole = [bar for _name, part in BAR2 for bar in part]       # (every bar's chords: the bass steps into the next)
    nxt = lambda bar: whole[(bar + 1) % len(whole)][0]
    rng = random.Random(2124)
    keys, bass = Voicer(52, 70, 4), Bass(28, 45)
    b0 = 0
    for k, (name, part) in enumerate(BAR2):
        if k:
            s.marker(b0 * s.beats, 'sect')     # (each later section's start a place the bar's song may begin)
        {'Dog Watch': dog_watch, 'Harbour Lights': harbour_lights, 'Salt Road': salt_road,
         'Galley Blues': galley_blues, 'Dog Watch again': dog_watch_again}[name](s, b0, part, nxt, keys, bass, rng)
        b0 += len(part)
    return s


def dog_watch(s, b0, part, nxt, keys, bass, rng):
    """I, D minor: the head on guitar and vibraphone in unison; the tine piano's chorus, the guitar's bridge"""
    for i, syms in enumerate(part):
        bar = b0 + i
        comp(s, 0, bar, syms, keys, 50 if 32 <= i < 48 else 58)
        walk2(s, 1, bar, syms, nxt(bar), bass, 80)
        dw_drums(s, bar, fill=i % 8 == 7, lift=i >= 48)
    for lead, vel in ((2, 86), (3, 66)):                  # the head: the guitar, the vibraphone softly with it
        s.tune(lead, b0, DW_HEAD_A, vel)
        s.tune(lead, b0 + 8, DW_HEAD_A2, vel)
        s.tune(lead, b0 + 16, DW_HEAD_B, vel)
        s.tune(lead, b0 + 24, DW_HEAD_A3, vel)
        s.tune(lead, b0 + 56, DW_HEAD_OUT, vel)
    put(s, 5, Tune(rng, 'D', 'minor', 62, 81).period(b0 + 32, part[32:48], SOLO, SOLO_END), 74)
    s.tune(2, b0 + 48, DW_HEAD_B, 84)


def harbour_lights(s, b0, part, nxt, keys, bass, rng):
    """II, A flat: a ballad, the band in two; the clarinet over held chords and soft strings, the guitar's bridge"""
    strings = Voicer(55, 74, 3, 16)
    for i, syms in enumerate(part):
        bar = b0 + i
        for h, x in enumerate(syms):
            c = chord(x)
            v = keys.next(c)
            s.play(0, bar, h * 2, 4 / len(syms) - 0.1, v, 50)
            s.play(10, bar, h * 2, 4 / len(syms) - 0.05, strings.next(c), 46, spread=2)
        if len(syms) == 1 and i % 2 == 1:
            s.play(0, bar, 2.5, 1.4, v[-2:], 40)                      # the top two again, softly
        two_feel(s, 1, bar, syms, nxt(bar), bass, 76)
        ballad_drums(s, bar)
    s.tune(8, b0, HB_TUNE_A, 84)
    s.tune(8, b0 + 8, HB_TUNE_A2, 82)
    s.tune(2, b0 + 16, HB_TUNE_B, 80)
    s.tune(8, b0 + 24, HB_TUNE_A3, 84)


def salt_road(s, b0, part, nxt, keys, bass, rng):
    """III, C minor, straight eighths: the montuno, the tumbao, the clave, coming in over eight bars under the
    clarinet (salt_road_way_in); the guitar's tune in octaves from the B part, a vibraphone chorus over the
    organ, the tune again"""
    s.swing = 0.5
    salt_road_way_in(s, b0)                                         # (first, in time order: Comp.note cuts
    piano, organ = Voicer(55, 72, 4), Voicer(50, 67, 3, 16)        # short the last note added at a pitch)
    n = len(part)
    for i, syms in enumerate(part):
        bar = b0 + i
        grow = min(1.0, 0.6 + 0.05 * i)                             # (the way in: the band from soft, full at
        montuno(s, 0, bar, syms, piano, 56 - 2 * max(0, 8 - i), i)  # the ninth bar, the clave from the fifth)
        tumbao(s, 1, bar, syms, nxt(bar), bass, 88 - 3 * max(0, 4 - i), first=i == 0, last=i == n - 1)
        latin_drums(s, bar, i, level=grow, rim=i >= 4)
        if 16 <= i < 32:
            s.play(4, bar, 0, 3.9, organ.next(chord(syms[0])), 56, spread=2)
    t = Tune(rng, 'C', 'minor', 62, 77)
    head = t.period(b0, part[:16], LATIN, LATIN_END)
    out = [x for x in shift(head, 32) if x[0] < b0 + 44] + t.phrase(b0 + 44, part[44:], LATIN, LATIN_END,
                                                                       shape='fall')
    first = sum(1 for x in head if x[0] < b0 + 8)                 # (the head's A part is the clarinet's: its
    unplayed(s, first)                                              # draws made, the notes left out)
    for x in head[first:]:                                          # (each note and the octave under it, in
        put(s, 2, [x], 84)                                          # turn: the octave of an earlier note no
        put(s, 2, transpose([x], -12), 72)                          # longer cuts off a later one)
    unplayed(s, first)
    put(s, 2, out, 84)
    put(s, 2, transpose(out, -12), 72)                              # the octave under it
    put(s, 3, Tune(rng, 'C', 'minor', 65, 82).period(b0 + 16, part[16:32], SOLO, SOLO_END), 74)
    s.swing = 0.64


def salt_road_way_in(s, b0):
    """the turn from the ballad into the latin (the user, at its start: "this kinda came out of nowhere...we need to
    connect it better to the rest of the predecessor song"): in the ballad's last bar a shaker comes up from
    nothing in straight eighths; over the first two latin bars the ballad's ride keeps the beat and its strings
    resolve (B, D, G to C, E flat, G) and hold, fading; the clarinet goes on and plays the ballad's tune into the
    latin time (its rising three notes with the last pushed ahead of the beat), falling at the end to the G the
    guitar's octaves take up at the B part.  The montuno, the bass and the kit come up over the eight bars
    (salt_road)"""
    with own_draws(s, 9601):
        for e in range(8):
            s.play(9, b0 - 1, e * 0.5, 0.2, 70, 6 + 2 * e, spread=2)
        for k, bar in enumerate((b0, b0 + 1)):
            for beat in range(4):
                s.play(9, bar, beat, 0.2, 51, (36 if beat % 2 == 0 else 32) - 6 * k, spread=3)
        s.play(10, b0, 0, 7.8, ['C4', 'Eb4', 'G4'], 44, spread=2)       # (over C minor and A flat both)
        s.ramp(10, b0, 2, 127, 56)
        s.cc(10, b0 + 2, 2, 11, 127)                                    # (back up once the strings are gone)
        s.tune(8, b0, SR_WAY_IN, 80)


def galley_blues(s, b0, part, nxt, keys, bass, rng):
    """IV, G: a jazz blues for an organ trio: the guitar's head, again with the vibraphone, the tine piano's
    chorus, the head out"""
    organ = Voicer(50, 68, 4)
    for i, syms in enumerate(part):
        bar = b0 + i
        comp(s, 4, bar, syms, organ, 58 if 24 <= i < 36 else 54)
        walk2(s, 1, bar, syms, nxt(bar), bass, 80)
        swing_drums(s, bar, lift=24 <= i < 36, fill=i % 12 == 11)
    t = Tune(rng, 'G', 'mixolydian', 62, 79)
    head = t.period(b0, part[:12], SWING, SWING_END)
    put(s, 2, head + shift(head, 12), 86)
    put(s, 3, shift(head, 12), 60)                                 # the vibraphone with it the second time
    put(s, 5, Tune(rng, 'G', 'mixolydian', 62, 81).period(b0 + 24, part[24:36], SOLO, SOLO_END), 76)
    put(s, 2, [x for x in shift(head, 36) if x[0] < b0 + 44], 86)
    put(s, 2, t.phrase(b0 + 44, part[44:], SWING, SWING_END, shape='fall'), 84)


def dog_watch_again(s, b0, part, nxt, keys, bass, rng):
    """V, D minor: the head on flute and guitar, the guitar's own bridge, the last eight on guitar and
    vibraphone as at the start, turning round into the top"""
    for i, syms in enumerate(part):
        bar = b0 + i
        comp(s, 0, bar, syms, keys, 54)
        walk2(s, 1, bar, syms, nxt(bar), bass, 80)
        dw_drums(s, bar, fill=i % 8 == 7)
    s.tune(7, b0, DW_HEAD_A, 74)
    s.tune(7, b0 + 8, DW_HEAD_A2, 74)
    s.tune(2, b0, DW_HEAD_A, 64)
    s.tune(2, b0 + 8, DW_HEAD_A2, 64)
    put(s, 2, Tune(rng, 'F', 'major', 60, 81).period(b0 + 16, part[16:24], SOLO, SOLO_END), 78)
    s.tune(2, b0 + 24, DW_HEAD_A3, 84)
    s.tune(3, b0 + 24, DW_HEAD_A3, 64)


# --- QUARTER2: "Quiet Hours" ---

QH_A = bars_of('Dbmaj9 | Dbmaj9 | Bbm9 | Bbm9 | Gbmaj9 | Gbmaj7#11 | Ebm9 | Ab9sus4')
QH_B = bars_of('Dbmaj9 | Fm7 | Bbm9 | Bbm9 | Gbmaj9 | Gbm6 | Dbmaj9/F | Ebm9 Ab9sus4')
QH_TUNE = ['Ab4:1:1 Eb5:2:2', 'F5:0:3', 'Eb5:0:1.5 Db5:1.5:.5 C5:2:2', '',
           'Bb4:1:1 Db5:2:1 F5:3:1', 'Ab5:0:2 F5:2:2', 'Gb5:0:1.5 F5:1.5:.5 Db5:2:2', 'Eb5:0:3',
           'Ab4:1:1 Eb5:2:2', 'Eb5:0:1.5 F5:1.5:.5 Ab5:2:2', 'F5:0:1.5 Eb5:1.5:.5 Db5:2:2', 'C5:0:3',
           'Bb4:1:1 Db5:2:1 F5:3:1', 'Eb5:0:3', 'Db5:0:1.5 C5:1.5:.5 Ab4:2:2', 'Bb4:0:3']
DR_A = bars_of('Bbm9 | Bbm9 | Gbmaj9 | Gbmaj9 | Ebm9 | Fm7 | Gbmaj7#11 | F9sus4')
DR_B = bars_of('Bbm9 | Dbmaj9 | Gbmaj9 | Ebm9 | Bbm9/F | Gbmaj7#11 | Ebm9 | Db9sus4')      # turning to G flat
# Drift's vibraphone over the water: long notes, each in its bar's pentatonic (nothing a half step from the water)
DR_TUNE = ['', 'F5:1:1 Ab5:2:2', 'Bb5:0:3 Ab5:3:1', 'Gb5:0:2 Eb5:2:2',
           'Db5:0:3', 'Eb5:1:1 F5:2:2', 'Ab5:0:2 Bb5:2:2', 'G5:0:2 F5:2:2',
           '', 'F5:0:2 Ab5:2:2', 'Gb5:0:2 Eb5:2:2', 'Db5:0:3',
           '', 'Bb5:0:3 Ab5:3:1', 'Gb5:0:2 Eb5:2:2', 'Db5:0:3']
# the water's ripples, a bar each: steps along the pentatonic, on and a little back, some a skip (rising four or
# five steps, some an octave; a falling bar mirrors)
RIPPLES = [(1, 2, -1, 1, 2, -1, 1), (2, -1, 2, -1, 2, -1, 1), (1, 1, 2, -1, 1, 1, -1), (2, 1, -1, 2, -1, 1, 1)]
LN_A = bars_of('Gbmaj9 | Abm9 | Bbm7 | Abm9 | Gbmaj9 | Cbmaj7#11 | Ebm9 | Db9sus4')
LN_B = bars_of('Gbmaj9 | Abm9 | Bbm7 | Ebm9 | Cbmaj7#11 | Cbm6 | Gbmaj9/Bb | Ab9sus4')   # turning to D flat
# a few notes a bar at most, long ones, and bars of nothing
DRIFT = [[(0, 3)], [(1, 1), (2, 2)], [(2, 2)], [], [(0, 2), (2, 1.5)], [(0, 1.5), (1.5, .5), (2, 2)], [(1, 3)]]
DRIFT_END = [[(0, 3)], [(0, 2)], [(0, 3.5)]]
# IV's second half: the second B flat minor turned to E flat (its G natural a colour, the tune's C its 13th), and
# the vibraphone above the tune in long notes, a bar each: A flat, G, then from B flat falling by half steps,
# A natural (over the G flat minor), A flat, G flat, into the first bar's F
QH_B_LATE = QH_B[:3] + bars_of('Eb9/Bb') + QH_B[4:]
QH_DESCANT = ['Ab5:1:3', 'G5:0:4', 'Bb5:0:4', 'A5:0:4', 'Ab5:0:4', 'Gb5:0:3']


def rolled(s, ch, bar, beat, dur, notes, vel, gap=0.12):
    """a chord rolled gently from the bottom, every note let go together"""
    for k, q in enumerate(notes):
        s.play(ch, bar, beat + k * gap, dur - k * gap, q, vel - 2 * k, spread=2)


def quarters2_song():
    s = Comp(54, seed=31)
    s.setup(0, 5, 104)      # electric piano, the warm one: the rolled chords
    s.setup(1, 89, 98)      # warm pad (wide)
    s.setup(2, 32, 104)     # upright bass, the downbeats only
    s.setup(3, 11, 92)      # vibraphone, now and then
    s.setup(4, 4, 104)      # electric piano with the tine: the tune
    s.setup(5, 46, 104)     # harp (II)
    s.setup(6, 8, 76)       # celesta: two notes in all
    s.setup(7, 73, 100)     # flute, low (III)
    s.setup(8, 24, 104)     # nylon guitar (III)
    s.setup(10, 90, 98)     # soft pad (II)
    rng = random.Random(1201)
    bar = quiet_hours(s, 0)
    for section in (drift, lantern, quiet_again):
        s.marker(bar * s.beats, 'sect')        # (the places the quarters' song may begin: src/game.c room_music)
        bar = section(s, bar, rng)
    return s


def calm_bed(s, b0, harmony, piano, pad, bass, keys_vel=50, pad_vel=56, bass_vel=58, thin=()):
    """the piano's chord rolled once a bar (each half, two chords), its top note again on the fourth beat of
    every other bar; the pad held; the bass on the downbeat.  thin: bars (from b0) with the piano's roll only"""
    for i, syms in enumerate(harmony):
        bar = b0 + i
        for h, x in enumerate(syms):
            c = chord(x)
            span = 4 / len(syms)
            v = piano.next(c)
            rolled(s, 0, bar, h * span, span - 0.15, v if i not in thin else v[:3], keys_vel)
            s.play(1, bar, h * span, span - 0.05, pad.next(c), pad_vel, spread=2)
            s.play(2, bar, h * span, span - 0.2, bass.note(c), bass_vel)
        if len(syms) == 1 and i % 2 == 1 and i not in thin:
            s.play(0, bar, 3, 0.9, v[-1], keys_vel - 8)


def quiet_hours(s, b0):
    """I, D flat: the rolled piano, the wide pad, the bass on the downbeat, the tine piano's tune"""
    calm_bed(s, b0, QH_A + QH_B, Voicer(53, 70, 4), Voicer(50, 69, 3, 16), Bass(28, 43))
    s.tune(4, b0, QH_TUNE, 70)
    return b0 + 16


def pentatonic(c):
    """the five notes a chord's water runs on, no half step among them: a major chord's own pentatonic, a minor
    chord's minor one, a suspended chord's from its flat seventh"""
    t = set(c.tones)
    base = c.root + (10 if 5 in t and not t & {3, 4} else 3 if 3 in t else 0)
    return {(base + x) % 12 for x in (0, 2, 4, 7, 9)}


def water(s, ch, b0, harmony, rng, lo=58, hi=74, vel=42):
    """running water (the user, of Drift: "a bit messy at this part...we could go for dripping pipe if we wanted or
    running water--if it's relaxing enough"): eighths on the chord's pentatonic, a bar rising in ripples (steps
    and skips on, a step back) and the next falling, each note let ring a beat and a quarter (not over a change of
    chord); louder on the beats and at the crest.  The line runs on over the bar lines (the nearest note of the
    next chord's scale); the last bar falls and comes to rest on its fourth beat"""
    q, n = lo, len(harmony)
    for i, syms in enumerate(harmony):
        up = i % 2 == 0
        steps = rng.choice(RIPPLES)
        last = i == n - 1
        for e in range(8 if not last else 7):
            beat = e * 0.5
            c = chord(syms[0] if beat < 2 or len(syms) == 1 else syms[1])
            ladder = [x for x in range(lo, hi + 1) if x % 12 in pentatonic(c)]
            k = min(range(len(ladder)), key=lambda j: abs(ladder[j] - q))
            if e:
                d = steps[e - 1] * (1 if up and not last else -1)
                if not 0 <= k + d < len(ladder):
                    d = -d                                          # (at the edge of the range: turn back)
                k += d
            q = ladder[k]
            crest = 1 - abs(e - (7 if up else 0)) / 14          # (the top of the wave a little louder)
            v = vel + (4 if beat in (0, 2) else 0) + round(4 * crest)
            turn = i + 1 < n and harmony[i + 1][0] != syms[-1]          # (no ringing over a change of chord)
            s.play(ch, b0 + i, beat, 1.0 if last and e == 6 else min(1.25, 4 - beat) if turn else 1.25, q, v,
                   spread=3)


def drift(s, b0, rng):
    """II, B flat minor: running water on the harp under the soft pad, the bass on the downbeat, the vibraphone's
    long notes above it, in the water's scales"""
    harmony = DR_A + DR_B
    Tune(rng, 'Bb', 'dorian', 65, 80).period(b0, harmony, DRIFT, DRIFT_END)   # (the old tune's draws, not played:
    pad, bass = Voicer(50, 69, 3, 16), Bass(28, 43)                            # Lantern's flute draws after them)
    for i, syms in enumerate(harmony):
        c = chord(syms[0])
        s.play(10, b0 + i, 0, 3.95, pad.next(c), 56, spread=2)
        s.play(2, b0 + i, 0, 3.8, bass.note(c), 54)
    water(s, 5, b0, harmony, random.Random(1216))
    s.tune(3, b0, DR_TUNE, 62)
    return b0 + 16


def lantern(s, b0, rng):
    """III, G flat: nylon guitar picked in quarters, the wide pad, a low flute, two far celesta notes"""
    harmony = LN_A + LN_B
    gtr, pad, bass = Voicer(55, 71, 3), Voicer(50, 69, 3, 16), Bass(28, 43)
    for i, syms in enumerate(harmony):
        bar = b0 + i
        c = chord(syms[0])
        v = gtr.next(c)
        low = next(q for q in range(40, 52) if q % 12 == c.bass)        # the guitar's own low string
        s.play(8, bar, 0, 3.8, low, 50)
        for k in range(3):
            s.play(8, bar, k + 1, 2.8 - k, v[k], 44 if k else 46)
        for h, x in enumerate(syms):
            s.play(1, bar, h * 2, 4 / len(syms) - 0.05, pad.next(chord(x)), 52, spread=2)
        s.play(2, bar, 0, 3.8, bass.note(c), 48)
    put(s, 7, Tune(rng, 'Gb', 'major', 63, 77).period(b0, harmony, DRIFT, DRIFT_END), 64)
    for bar, q in ((b0 + 5, 'Bb6'), (b0 + 12, 'Db7')):                  # far off, once each
        s.play(6, bar, 2, 2, q, 34)
    return b0 + 16


def broken_bed(s, b0, harmony, piano, pad, bass, keys_vel, pad_vel, bass_vel):
    """calm_bed's chords broken across the bar, a note a beat from the bottom, each held to the bar's end"""
    for i, syms in enumerate(harmony):
        c = chord(syms[0])
        v = piano.next(c)
        for k, q in enumerate(v):
            s.play(0, b0 + i, k, 3.85 - k, q, keys_vel + (4 if k == 0 else 0), spread=2)
        s.play(1, b0 + i, 0, 3.95, pad.next(c), pad_vel, spread=2)
        s.play(2, b0 + i, 0, 3.8, bass.note(c), bass_vel)


def quiet_again(s, b0, rng):
    """IV, D flat: the tune on the vibraphone over the rolled chords; from the seventh bar (4:00 into the song; the
    user: "make a bit less lethargic towards the end") the piano breaks its chords a note a beat, the tune's second
    half is played whole on the tine piano, its second B flat minor turned to E flat, and the vibraphone rises
    above it in long notes falling by half steps ("we need some notes of color"); the last bar rolled again, into
    the first"""
    harmony = QH_A + QH_B_LATE
    piano, pad, bass = Voicer(53, 70, 4), Voicer(50, 69, 3, 16), Bass(28, 43)
    calm_bed(s, b0, harmony[:6], piano, pad, bass, keys_vel=48, pad_vel=54, bass_vel=54)
    broken_bed(s, b0 + 6, harmony[6:15], piano, pad, bass, keys_vel=48, pad_vel=54, bass_vel=54)
    calm_bed(s, b0 + 15, harmony[15:], piano, pad, bass, keys_vel=46, pad_vel=52, bass_vel=52)
    s.tune(3, b0, QH_TUNE[:8], 64)
    s.tune(4, b0 + 8, QH_TUNE[8:], 62)
    s.tune(3, b0 + 10, QH_DESCANT, 54)
    return b0 + 16


SONGS = {'BAR2.MID': bar2_song, 'QUARTER2.MID': quarters2_song}
