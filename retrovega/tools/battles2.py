"""Four more battles for the third draft's orchestral score (CC0, ours): BATTLE4..7, to keep the fights fresh.

The user (2026-11-17, in the soundtrack review), of each battle: Contact "We need another combat piece like this in
addition to keep it fresh"; Break and Attack "More battle music like this with some downbeat notes of
apprehension"; Furball "more battle music would be great to keep it fresh".  So one more in each one's vein, each
its own; and then, of Check Six, "one last battle piece like check six":

    BATTLE4  "Intercept"   Contact's vein: a call answered over the driving spiccato.  The synth brass calls
                           (a short note and a leap up to a long one, the leap wider each time), the two horns
                           answer it a fifth lower in thirds; over the C pedal, the low strings galloping; then the
                           strings' long tune soaring to C over moving chords, a darkened chorale under a high C
                           and a build into the dialogue tutti
    BATTLE5  "Check Six"   Break and Attack's drive (the motor at full speed under half-time hits, a riff of its own
                           on spiccato strings in octaves) with apprehension on every downbeat: the bass falling by
                           half steps (C, B, B flat, A flat, G) held by the low horn and the strings, the timpani on
                           each one, the brass's sighs (a half step down from each downbeat), the horn's tune
                           over the riff's strings driving, a darker turn into E flat minor, and last the sighs
                           soaring in long notes on the strings and the horn over the riff
    BATTLE6  "Scissors"    Furball's vein: a riff over driving fifths with the phrygian D flat in it.  The riff is
                           two blades: the brass climbing and the horn falling from one C, opening to the octave
                           and closing on the 3+3+2, then a run in contrary motion snipping shut; the horn's tune
                           over the descent F minor, E flat, D flat, C; the blades drawn out in long notes over
                           A flat, B flat, G7 to C minor; a climb into a stop on F minor
    BATTLE7  "Closing In"  Check Six's sounds and its dread on the downbeats, the other way up: the bass climbing a
                           half step every bar (C up to G, and on into A flat minor) held by the low horn and the
                           strings, the timpani on each one, under a riff of its own holding C on every downbeat
                           (its C and four dotted eighths across the beats, 4+3+3+3+3, the hits on one and the and
                           of three); the horn's tune in long notes over the climb (a call up a fourth, then the
                           same call a half step higher with the bass); home up whole steps; the call passed
                           between the brass and the horn down the circle of fifths; and last the tune on the
                           strings and the horn over the riff

The lego contract is battles.py's, kept to the letter: 140 in 4/4 in C minor's family, the spiccato motor in every
bar, a "sect" marker every four bars of the body, each block starting on its downbeat with the motor, a crash and
a hit and standing alone, ending on C or G (its last bar may fill), and no note held over its end.  Every channel's
expression is set at the start, and set full again at each block's downbeat on the channels that swell, so a cut
into any block finds the parts as the piece has them there.
"""

from compose import chord
from songs import p
from orch import NAMES, DRUM, Piece, up, scale_run, ACC332, in_range, hit, held, sung, in_time
from battles import (BPM, DIM7_G, begin, motor, block_pad, fill, run, timp_roll, halftime, fifths)


def fresh(s, bars, chans, every=4):
    """each block's downbeat: these channels' expression full again (written before the block's swells, which come
    after it at the same tick and so win)"""
    for b in range(bars.start, bars.stop, every):
        for c in chans:
            s.cc(c, b, 0, 11, 127)


def toms(s, bar, beat=3, vel=84, step=.25, n=4):
    """a tom run down from this beat to the bar's end"""
    names = ('tom1', 'tom1', 'tom2', 'tom2', 'tom3', 'tom3', 'tom4', 'tom4') if n == 8 else ('tom1', 'tom2', 'tom3',
                                                                                           'tom4')
    for k, t in enumerate(names):
        s.play(9, bar, beat + k * step, .2, DRUM[t], vel + k * 3, 2)


def nm(q):
    return '%s%d' % (NAMES[q % 12], q // 12 - 1)


def landing(s, ch, bar, text, vel):
    """notes that land on the downbeat with their weight and die away (expression from full down): the
    apprehension under Check Six"""
    for i, t in enumerate(text):
        for tok in t.split():
            name, beat, dur = tok.split(':')
            held(s, ch, bar + i, float(beat), float(dur), name, vel, e0=127, e1=104, e2=58)


# --- BATTLE4: "Intercept" (C minor over a C pedal, 140) -----------------------------
# Contact's way (a call answered over the motor) with its own call and colours: the synth brass calls first and the
# two horns answer in thirds, a fifth lower, each call a short note leaping to a long one (a minor sixth, then a
# major sixth); the motor's figure keeps going back to the voicing's top note, the low strings gallop (an eighth
# and two sixteenths).  Then the strings' long tune over moving chords, the horn and the brass holding, the strings'
# bass in long notes; then a chorale in the dark (F minor, D flat, B flat minor, C) under a high C, and a build by steps
# into the dialogue for the whole orchestra; the tune again with the brass under it, and a tom run into the loop.

IC_A1 = ['Cm', 'Gm/C', 'Fm/C', 'Cm']
IC_A2 = ['Db/C', 'Ab/C', 'Bb/C', 'Gsus4 G']
IC_B1 = ['Ab', 'Bb', 'Gm', 'Cm']
IC_B2 = ['Ab', 'Fm', 'Db', 'Gsus4 G']
IC_C1 = ['Fm', 'Db', 'Bbm', 'C']
IC_C2 = ['Ab', 'Bb', 'Cm', 'Gsus4 G']
IC_CALL = ['G4:0:.5 Eb5:.5:2.5 D5:3:.5 C5:3.5:.5', 'D5:0:2 Bb4:2:1 G4:3:1', '', '',
           'Ab4:0:.5 F5:.5:2.5 Eb5:3:.5 Db5:3.5:.5', 'C5:0:2 Bb4:2:1 Ab4:3:1', '', '']
IC_ANSWER = ['', '', 'C4:0:.5 Ab4:.5:2.5 G4:3:.5 F4:3.5:.5', 'G4:0:2 Eb4:2:1 C4:3:1',
             '', '', 'F4:0:.5 D5:.5:2.5 C5:3:.5 Bb4:3.5:.5', 'C5:0:2 B4:2:2']
IC_ANSWER2 = ['', '', 'Ab3:0:.5 F4:.5:2.5 Eb4:3:.5 C4:3.5:.5', 'Eb4:0:2 C4:2:1 G3:3:1',      # (a third under)
              '', '', 'D4:0:.5 Bb4:.5:2.5 Ab4:3:.5 F4:3.5:.5', 'G4:0:4']
IC_TUNE = ['C5:0:1.5 Eb5:1.5:.5 Ab5:2:2', 'G5:0:1 F5:1:1 D5:2:2', 'Eb5:0:2 D5:2:1 Bb4:3:1', 'C5:0:4',
           'C5:0:1.5 Eb5:1.5:.5 Ab5:2:1.5 G5:3.5:.5', 'Ab5:0:2 C6:2:2', 'Bb5:0:2 Ab5:2:1 F5:3:1',
           'G5:0:2 F5:2:1 D5:3:1']
IC_HOLD = ['Eb4:0:4', 'D4:0:4', 'D4:0:4', 'Eb4:0:4', 'Eb4:0:4', 'F4:0:4', 'F4:0:4', 'D4:0:2 B3:2:2']
IC_LOW = [(0, .45, 'r'), (.5, .2, 'r'), (.75, .2, 'r'), (1, .45, '8'), (1.5, .45, 'r'),
          (2, .45, 'r'), (2.5, .2, 'r'), (2.75, .2, 'r'), (3, .45, '5'), (3.5, .45, '8')]   # the gallop
IC_TURN = [0, 3, 1, 3, 2, 3, 1, 3]


def intercept():
    form = ['Cm', 'Cm'] + IC_A1 + IC_A2 + IC_B1 + IC_B2 + IC_C1 + IC_C2 + IC_A1 + IC_A2 + IC_B1 + IC_B2
    s = Piece(BPM, 1212, form, level=1.0)
    ch = begin(s)
    mo, hi_, lo, br, hn, hn2, pc, tp, st, bs = (ch[k] for k in ('motor', 'hit', 'low', 'brass', 'horn', 'horn2',
                                                                'piccolo', 'timpani', 'strings', 'bass'))
    n = len(form)                                   # 2 + 40 = 42 bars, 72 s
    A1, A2, B1, B2, C1, C2, D1, D2, E1, E2 = (2 + 4 * k for k in range(10))
    s.loop_at(2)
    s.sections(2, n)
    fresh(s, range(2, n), (hn, hn2, br, st))
    # the motor everywhere (quieter in the dark chorale), the gallop but there
    motor(s, mo, range(0, C1), IC_TURN, 58)
    motor(s, mo, range(C1, C1 + 4), IC_TURN, 48)
    motor(s, mo, range(C2, n), IC_TURN, 58)
    s.ramp(mo, 0, 2, 60, 127)                       # the intro: the motor and the gallop coming up, a roll
    s.bass(lo, [b for b in range(0, n) if not C1 <= b < C1 + 4], IC_LOW, 82, lo=36, hi=47)
    timp_roll(s, tp, 1, 0, 4, 'G1', 10, 96)
    # the dialogue: the brass calls, the horns answer in thirds (then the whole orchestra: the call on brass with
    # the horn an octave under, the answer on the strings an octave up with the second horn, the brass on its third)
    for P, tutti in ((A1, False), (D1, True)):
        for blk in (P, P + 4):
            s.crash(blk, 0, 104 if blk == P else 96)
            for b, beats in ((blk, ACC332 if tutti else (0, 1.5, 3)), (blk + 1, (0,) if not tutti else (0, 1.5)),
                             (blk + 2, (0, 1.5) if tutti else (0,)), (blk + 3, (0,) if not tutti else (0, 1.5))):
                for beat in beats:
                    hit(s, hi_, b, beat, 110 if beat == 0 else 100, timpani=tp if beat == 0 else None)
            for b in (blk + 1, blk + 3):            # the timpani galloping between
                s.line(tp, b, 'C2:2:.5 C2:2.5:.25 C2:2.75:.25 G1:3:.5 G1:3.5:.25', 76)
        s.tune(br, P, IC_CALL, 90 if not tutti else 94)
        s.tune(hn, P, IC_ANSWER, 84)
        s.tune(hn2, P, IC_ANSWER2, 76)
        if tutti:
            s.tune(hn, P, [up(t, -12) if t else '' for t in IC_CALL], 80)
            s.tune(st, P, [up(t, 12) if t else '' for t in IC_ANSWER], 74)
            s.tune(br, P, [up(t, 12) if t else '' for t in IC_ANSWER2], 76)
            s.tune(pc, P + 4, [up(t, 12) for t in IC_CALL[4:6]], 50)
        run(s, pc, P + 7, 2, DIM7_G, 0.25, 58, 2)
        timp_roll(s, tp, P + 7, 2, 2, 'G1', 20, 92)
    # the strings' tune over moving chords, the horn holding a line and the brass the chords, the strings' bass in
    # long notes (the second time the brass on the tune an octave down, and the hits on the 3+3+2)
    for P, second in ((B1, False), (E1, True)):
        for blk in (P, P + 4):
            s.crash(blk, 0, 100 if blk == P else 92)
            for b in range(blk, blk + 4):
                hit(s, hi_, b, 0, 108 if b == blk else 100, timpani=tp)
                if second and b % 2 == 1:
                    for beat in (1.5, 3):
                        hit(s, hi_, b, beat, 98)
                elif b % 2 == 1:
                    hit(s, hi_, b, 2.5, 96)
                s.play(bs, b, 0, 3.9, in_range(chord(s.sym_at(b, 0)).bass, 36, 47), 80)
        s.tune(st, P, IC_TUNE, 94 if not second else 96)
        sung(s, hn, P, IC_HOLD, 80 if not second else 82)
        if not second:
            block_pad(s, br, range(P, P + 8), 57, 70, 3, 58)
        else:
            s.tune(br, P, [up(t, -12) for t in IC_TUNE], 86)
            s.tune(pc, P + 5, [up(IC_TUNE[5], 12)], 52)
    timp_roll(s, tp, B2 + 3, 0, 4, 'G1', 12, 96)
    toms(s, B2 + 3, 3, 82)
    timp_roll(s, tp, E2 + 3, 0, 4, 'G1', 12, 100)
    run(s, pc, E2 + 3, 2, DIM7_G, 0.25, 60, 2)
    toms(s, E2 + 3, 2, 84, n=8)
    # the dark chorale: the horns in two parts, a high C held over it, the low strings on the 3+3+2, the timpani
    s.crash(C1, 0, 96)
    hit(s, hi_, C1, 0, 104, timpani=tp)
    sung(s, hn, C1, ['Ab4:0:4', 'Ab4:0:4', 'F4:0:4', 'E4:0:4'], 82, e0=50, e1=127, e2=90)
    sung(s, hn2, C1, ['C4:0:4', 'F4:0:4', 'Db4:0:4', 'C4:0:4'], 76, e0=50, e1=127, e2=90)
    for b in range(C1, C1 + 4):
        s.play(st, b, 0, 3.9, 'C6', 52)
        for beat in ACC332:
            s.play(lo, b, beat, 1.2 if beat < 3 else .8, in_range(chord(s.sym_at(b, beat)).bass, 36, 47),
                   80 + (8 if beat == 0 else 0))
        s.play(tp, b, 0, .5, in_range(chord(s.sym_at(b, 0)).bass, 33, 44), 82)
        s.play(tp, b, 2, .5, in_range(chord(s.sym_at(b, 0)).bass, 33, 44), 64)
    # the build: the brass climbing a step a bar over the horns, the hits every bar, a roll into the tutti
    s.crash(C2, 0, 100)
    sung(s, br, C2, ['C5:0:4', 'D5:0:4', 'Eb5:0:4', 'F5:0:2 D5:2:2'], 86, e0=70, e1=127, e2=110)
    sung(s, hn, C2, ['Ab3:0:4', 'Bb3:0:4', 'C4:0:4', 'C4:0:2 B3:2:2'], 78)
    sung(s, hn2, C2, ['Eb4:0:4', 'F4:0:4', 'G4:0:4', 'G4:0:4'], 72)
    for k, b in enumerate(range(C2, C2 + 4)):
        hit(s, hi_, b, 0, 102 + 3 * k, timpani=tp)
    hit(s, hi_, C2 + 3, 1.5, 108)
    timp_roll(s, tp, C2 + 3, 0, 4, 'G1', 14, 104)
    run(s, pc, C2 + 3, 2, DIM7_G, 0.25, 58, 2)
    fill(s, C2 + 3, 84)
    s.end(n)
    return s


# --- BATTLE5: "Check Six" (C minor, 140) -------------------------------------------
# Break and Attack's drive: the motor at full speed under hits on one and three, a riff of its own on spiccato
# strings in octaves (3+3+2 in sixteenths, circling C through B), brass calls answered by the riff's strings.  And
# apprehension on every downbeat: the bass falls by half steps (C, B, B flat, A flat, G: the old lament), each new
# note held by the low horn and the strings and struck by the timpani, landing and dying away; the brass's calls
# are sighs, a half step above where they fall to; the horn's tune is over F minor with the low notes falling,
# the riff's strings driving under it in the riff's rhythm; then darker, the riff and the sighs in E flat minor,
# and back: the riff, then over it the sighs soaring in long notes.

CS_R1 = ['Cm', 'G/B', 'Cm/Bb', 'Fm/Ab G']
CS_R2 = ['Cm', 'G/B', 'Cm/Bb', 'Db G']
CS_K1 = ['Cm', 'G', 'Fm', 'Cm']
CS_K2 = ['Ab', 'Fm', 'Db', 'Gsus4 G']
CS_N1 = ['Fm', 'Db', 'Bbm', 'C']
CS_N2 = ['Fm', 'Ab', 'Db', 'G']
CS_D1 = ['Ebm', 'Bb/D', 'Ebm/Db', 'Abm/B Bb']
CS_D2 = ['Ab', 'Db', 'Fm', 'Gsus4 G']
CS_RIFF = ['C5:0:.75 B4:.75:.75 C5:1.5:.5 Eb5:2:.75 D5:2.75:.75 C5:3.5:.5',
           'D5:0:.75 B4:.75:.75 G4:1.5:.5 Ab4:2:.75 G4:2.75:.75 F4:3.5:.5',
           'Eb5:0:.75 D5:.75:.75 Eb5:1.5:.5 G5:2:.75 F5:2.75:.75 Eb5:3.5:.5',
           'F5:0:.75 Eb5:.75:.75 C5:1.5:.5 B4:2:.75 D5:2.75:.75 G5:3.5:.5',
           'C5:0:.75 B4:.75:.75 C5:1.5:.5 Eb5:2:.75 D5:2.75:.75 C5:3.5:.5',
           'D5:0:.75 B4:.75:.75 G4:1.5:.5 D5:2:.75 F5:2.75:.75 Ab5:3.5:.5',
           'G5:0:.75 F5:.75:.75 Eb5:1.5:.5 D5:2:.75 Eb5:2.75:.75 C5:3.5:.5',
           'Db5:0:.75 F5:.75:.75 Ab5:1.5:.5 G5:2:1 D5:3:.5 B4:3.5:.5']
CS_FALL = ['C3:0:4', 'B2:0:4', 'Bb2:0:4', 'Ab2:0:2 G2:2:2']          # the bass by half steps, held
CS_SIGH = ['Db5:0:2 C5:2:2', 'Ab4:0:2 G4:2:1 B4:3:1', 'Gb4:0:2 F4:2:1 Ab4:3:1', 'Ab4:0:2 G4:2:2',
           'Bb4:0:2 Ab4:2:1 C5:3:1', 'Db5:0:2 C5:2:1 F5:3:1', 'Eb5:0:2 Db5:2:1 F5:3:1', 'Ab5:0:2 G5:2:2']
CS_REPLY = ['Eb5:2:.75 D5:2.75:.75 C5:3.5:.5', 'D5:2:.75 F5:2.75:.75 Ab5:3.5:.5', 'C5:2:.75 Eb5:2.75:.75 F5:3.5:.5',
            'G5:2:.75 F5:2.75:.75 Eb5:3.5:.5', 'Eb5:2:.75 F5:2.75:.75 Ab5:3.5:.5', 'Ab5:2:.75 G5:2.75:.75 F5:3.5:.5',
            'F5:2:.75 Ab5:2.75:.75 Db6:3.5:.5', 'B5:2:.75 G5:2.75:.75 D5:3.5:.5']
CS_HORN = ['C5:0:3 Ab4:3:1', 'F4:0:2 Ab4:2:1 Db5:3:1', 'F5:0:3 Db5:3:1', 'E5:0:2 C5:2:2',
           'C5:0:1.5 Db5:1.5:.5 C5:2:1 Ab4:3:1', 'Ab4:0:2 C5:2:1 Eb5:3:1', 'Db5:0:2 F5:2:2', 'D5:0:2 B4:2:2']
CS_UNDER = ['F3:0:4', 'F3:0:4', 'F3:0:4', 'E3:0:4', 'C3:0:4', 'C3:0:4', 'Ab2:0:4', 'G2:0:4']
CS_DARK = ['E5:0:2 Eb5:2:2', 'Eb5:0:2 D5:2:2', 'Eb5:0:2 Db5:2:2', 'B4:0:2 Bb4:2:2']
CS_CLIMB = ['Eb5:0:3 C5:3:1', 'Db5:0:2 F5:2:2', 'Ab5:0:2 G5:2:1 F5:3:1', 'G5:0:2 D5:2:1 B4:3:1']
CS_SOAR = ['Ab5:0:1.5 G5:1.5:2.5', 'C6:0:1.5 B5:1.5:2.5', 'Eb6:0:1.5 D6:1.5:1 C6:2.5:1.5', 'Db6:0:2 B5:2:2']
CS_LOW = [(0, .45, 'r'), (.5, .45, 'r'), (1, .45, '8'), (1.5, .45, 'r'), (2, .45, 'r'), (2.5, .45, 'r'),
          (3, .45, '8'), (3.5, .45, '5')]
CS_PULSE = (0, .75, 1.5, 2, 2.75, 3.5)              # the riff's own rhythm (3+3+2 in sixteenths, twice a bar)
CS_TURN = [2, 1, 0, 1, 3, 1, 0, 1]


def check_six():
    form = ['Cm'] + CS_R1 + CS_R2 + CS_K1 + CS_K2 + CS_N1 + CS_N2 + CS_D1 + CS_D2 + CS_R1 + CS_R2
    s = in_time(Piece(BPM, 1313, form, level=0.2))
    riff = 12                                       # (the riff's own strings, spiccato in octaves: Break and
    s.setup(riff, 44, 110)                          # Attack's)
    ch = begin(s, extra=(riff,))
    mo, hi_, lo, br, hn, hn2, pc, tp, st, bs = (ch[k] for k in ('motor', 'hit', 'low', 'brass', 'horn', 'horn2',
                                                                'piccolo', 'timpani', 'strings', 'bass'))
    n = len(form)                                   # 1 + 40 = 41 bars, 70.3 s
    R1, R2, K1, K2, N1, N2, D1, D2, Q1, Q2 = (1 + 4 * k for k in range(10))
    s.loop_at(1)
    s.sections(1, n)
    fresh(s, range(1, n), (hn, hn2, br, bs, st))
    motor(s, mo, range(0, n), CS_TURN, 58)
    s.bass(lo, range(0, n), CS_LOW, 82, lo=36, hi=47)
    timp_roll(s, tp, 0, 0, 4, 'C2', 10, 92)
    # the downbeats: every bar's new bass note held by the low horn and the strings, struck by the timpani
    def downbeats(blk, notes, horn_vel=84, strings=True):
        landing(s, hn2, blk, notes, horn_vel)
        if strings:
            landing(s, bs, blk, [up(t, -12) for t in notes], 78)
        for i, t in enumerate(notes):
            for tok in t.split():
                name, beat, _dur = tok.split(':')
                s.play(tp, blk + i, float(beat), .6, in_range(p(name), 31, 44), 92 if beat == '0' else 82)
    # the riff, half-time hits under it, the fall under that
    for blk, text, vel in ((R1, CS_RIFF[:4], 92), (R2, CS_RIFF[4:], 92), (Q1, CS_RIFF[:4], 94),
                           (Q2, CS_RIFF[4:], 92)):
        s.crash(blk, 0, 100 if blk in (R1, Q1) else 94)
        s.tune(riff, blk, text, vel)
        s.tune(riff, blk, [up(t, -12) for t in text], vel - 8)
        downbeats(blk, CS_FALL, strings=blk != Q2)        # (Q2: its low strings' voices to the tune)
        halftime(s, hi_, range(blk, blk + 4), 110 if blk < K1 else 112)
    # the last time a tune in long notes over the riff: the sighs soaring (each downbeat a half step above where
    # it falls, the heads climbing A flat, C, E flat, then D flat down to the leading note) on the strings, the
    # horn an octave under; no riff doubled (2026-11-17, the user: at 1:05 "this sounds a bit too much like
    # gameboy "boss music"": the riff had been up an octave with the brass and the horn on it in octaves, a
    # piccolo's arpeggio after, and its hits, timpani and landing notes cut to a tick: in_time)
    sung(s, st, Q2, CS_SOAR, 94, e0=90, e1=127, e2=104)
    sung(s, hn, Q2, [up(CS_SOAR[0], -12), 'G4:0:4'] + [up(t, -12) for t in CS_SOAR[2:]], 88, e0=90, e1=127,
         e2=104)                                    # (G/B: the horn on G, not the C a step over the riff's B)
    timp_roll(s, tp, Q2 + 3, 2, 2, 'G1', 20, 100)
    toms(s, Q2 + 3, 3, 86)
    toms(s, R2 + 3, 3, 80)
    # the brass's sighs, the horn an octave under, the riff's strings replying
    for blk in (K1, K2):
        s.crash(blk, 0, 100)
        halftime(s, hi_, range(blk, blk + 4), 106)
    s.tune(br, K1, CS_SIGH, 90)
    s.tune(hn, K1, [up(t, -12) for t in CS_SIGH], 78)
    s.tune(riff, K1, CS_REPLY, 82)
    s.tune(riff, K1, [up(t, -12) for t in CS_REPLY], 74)
    for b in range(K1, K1 + 8):
        s.play(tp, b, 0, .6, in_range(chord(s.sym_at(b, 0)).bass, 33, 44), 92)
    timp_roll(s, tp, K2 + 3, 0, 4, 'G1', 14, 96)
    toms(s, K2 + 3, 3, 82)
    # the horn's tune over F minor, the low notes falling under it, the brass holding the chords; under them the
    # riff's strings driving in the riff's own rhythm on each chord's bass in octaves, short, and the hits as the
    # riff's are (one and three, the snare on three) (2026-11-17, the user, at 0:38: "got a bit slow paced
    # here--love the sounds though": the riff's strings had stopped and the hits gone half as many)
    for blk in (N1, N2):
        s.crash(blk, 0, 104)
        halftime(s, hi_, range(blk, blk + 4), 108)
        for b in range(blk, blk + 4):
            for beat in CS_PULSE:
                q = in_range(chord(s.sym_at(b, beat)).bass, 48, 59)
                s.play(riff, b, beat, .2, [q, q + 12], 84 + (8 if beat in (0, 2) else 0), 3)
    sung(s, hn, N1, CS_HORN, 92, e0=80, e1=127, e2=110)
    block_pad(s, br, range(N1, N1 + 8), 57, 70, 2, 60)      # (two: the riff's strings' voices)
    downbeats(N1, CS_UNDER[:4], 80, strings=False)
    downbeats(N2, CS_UNDER[4:], 80, strings=False)
    run(s, pc, N2 + 3, 2, DIM7_G, 0.25, 58, 2)
    timp_roll(s, tp, N2 + 3, 2, 2, 'G1', 20, 98)
    # darker: the riff and the sighs in E flat minor, the fall with them; then the brass's climb home over the
    # riff's strings pulsing, hits on the 3+3+2, a roll and the toms
    s.crash(D1, 0, 108)
    s.tune(riff, D1, [up(t, 3) for t in CS_RIFF[:4]], 90)
    s.tune(riff, D1, [up(t, -9) for t in CS_RIFF[:4]], 82)
    sung(s, br, D1, CS_DARK, 88, longest=2, e0=110, e1=127, e2=80)
    s.tune(hn, D1, [up(t, -12) for t in CS_DARK], 76)
    downbeats(D1, [up(t, 3) for t in CS_FALL])
    halftime(s, hi_, range(D1, D1 + 4), 110)
    s.crash(D2, 0, 108)
    sung(s, br, D2, CS_CLIMB, 92, e0=90, e1=127, e2=110)
    s.tune(hn, D2, [up(t, -12) for t in CS_CLIMB], 80)
    for b in range(D2, D2 + 4):
        for k in range(8):
            q = in_range(chord(s.sym_at(b, k * .5)).root, 60, 71)
            s.play(riff, b, k * .5, .35, [q, q + 12], 68 + (8 if k % 2 == 0 else 0), 3)
        s.play(tp, b, 0, .5, in_range(chord(s.sym_at(b, 0)).bass, 33, 44), 90)
    halftime(s, hi_, range(D2, D2 + 4), 110, full=True)
    timp_roll(s, tp, D2 + 3, 0, 4, 'G1', 12, 100)
    toms(s, D2 + 3, 2, 86, n=8)
    s.end(n)
    return s


# --- BATTLE6: "Scissors" (C phrygian, 140) -----------------------------------------
# Furball's way (a riff over the low strings' fifths in eighths, the motor machine-gunning the power chords, the
# phrygian D flat) with a riff of its own: two blades from one C, the brass climbing through D flat and E flat while
# the horn falls through B flat and A flat, opening to the octave on the 3+3+2 and closing again, then opening wider
# and snipping shut in a run in contrary motion.  Then the horn's tune over the descent F minor, E flat, D flat to
# C (the phrygian cadence), high strings falling a step a bar over it; the blades drawn out (the brass climbing to
# G, the horn falling to B, over A flat, B flat, G7 and home to C minor); the climb a step a bar into a stop on the
# 3+3+2 on F minor, D flat to C, toms; the riff for everyone, the tune again.

SC_W = ['C5', 'Db5', 'C5', 'Ab5 Db5']
SC_X = ['Ab', 'Bb', 'G7', 'Cm']
SC_Y = ['Db', 'Eb', 'Fm', 'Db C']
SC_T1 = ['Fm', 'Eb', 'Db', 'C']
SC_T2 = ['Fm', 'Eb', 'Db', 'Db C']
SC_UPPER = ['C5:0:1.5 Db5:1.5:1.5 Eb5:3:1', 'F5:0:1.5 Eb5:1.5:1.5 Db5:3:1', 'C5:0:1.5 Eb5:1.5:1.5 G5:3:1',
            'Ab5:0:.5 G5:.5:.5 F5:1:.5 Eb5:1.5:.5 Db5:2:1 C5:3:1']
SC_LOWER = ['C5:0:1.5 Bb4:1.5:1.5 Ab4:3:1', 'F4:0:1.5 Ab4:1.5:1.5 Bb4:3:1', 'C5:0:1.5 Ab4:1.5:1.5 F4:3:1',
            'Eb4:0:.5 F4:.5:.5 G4:1:.5 Ab4:1.5:.5 Bb4:2:1 C5:3:1']
SC_TUNE = ['F4:0:1.5 Ab4:1.5:.5 C5:2:2', 'Bb4:0:3 G4:3:1', 'Ab4:0:1.5 F4:1.5:.5 Ab4:2:1 Db5:3:1', 'E5:0:3 C5:3:1',
           'F5:0:2 Eb5:2:1 C5:3:1', 'Bb4:0:1.5 C5:1.5:.5 Eb5:2:2', 'Db5:0:2 C5:2:1 Ab4:3:1', 'F4:0:2 E4:2:2']
SC_HIGH = ['C6:0:4', 'Bb5:0:4', 'Ab5:0:4', 'G5:0:4', 'C6:0:4', 'Bb5:0:4', 'Ab5:0:4', 'Ab5:0:2 G5:2:2']
SC_MID = ['Ab3:0:4', 'G3:0:4', 'F3:0:4', 'E3:0:4', 'Ab3:0:4', 'G3:0:4', 'F3:0:4', 'F3:0:2 E3:2:2']
SC_OPEN_UP = ['C5:0:1.5 Eb5:1.5:2.5', 'D5:0:1.5 F5:1.5:2.5', 'G5:0:3 F5:3:1', 'Eb5:0:4']    # the blades drawn out
SC_OPEN_DOWN = ['C5:0:1.5 Ab4:1.5:2.5', 'Bb4:0:1.5 F4:1.5:2.5', 'D4:0:3 B3:3:1', 'C4:0:4']
SC_CLIMB = ['F4:0:3 Ab4:3:1', 'G4:0:3 Bb4:3:1']
SC_TURN = [0, 1, 2, 1, 0, 2, 1, 2]


def scissors():
    form = (['C5', 'C5'] + SC_W + SC_W + SC_T1 + SC_T2 + SC_W + SC_X + SC_Y + SC_W + SC_T1 + SC_T2)
    s = in_time(Piece(BPM, 1414, form, level=-0.8))
    ch = begin(s)
    mo, hi_, lo, br, hn, hn2, pc, tp, st = (ch[k] for k in ('motor', 'hit', 'low', 'brass', 'horn', 'horn2',
                                                            'piccolo', 'timpani', 'strings'))
    n = len(form)                                   # 2 + 40 = 42 bars, 72 s
    W1, W2, T1, T2, W3, X, Y, W4, T3, T4 = (2 + 4 * k for k in range(10))
    s.loop_at(2)
    s.sections(2, n)
    fresh(s, range(2, n), (hn, hn2, br, st))
    stop = lambda b, pos: b == Y + 2 and pos not in ACC332          # the stop: on the 3+3+2 only
    motor(s, mo, [b for b in range(1, n) if b != Y + 2], SC_TURN, 58, lo=55, hi=74, n=3)
    fifths(s, lo, range(0, n), 82, skip=stop)
    s.ramp(mo, 1, 1, 70, 127)                       # the intro: the fifths alone, then the motor and a roll
    timp_roll(s, tp, 1, 0, 4, 'C2', 10, 96)
    # the riff: the blades, brass over horn (more of the orchestra on it each time)
    for blk, semis, more in ((W1, 0, 0), (W2, 0, 1), (W3, 0, 1), (W4, 0, 2)):
        upper = [up(t, semis) for t in SC_UPPER[:3]] + [SC_UPPER[3]]
        lower = [up(t, semis) for t in SC_LOWER[:3]] + [SC_LOWER[3]]
        s.crash(blk, 0, 104 if more < 2 else 110)
        s.tune(br, blk, upper, 90 if more < 2 else 94)
        s.tune(hn, blk, lower, 84 if more < 2 else 88)
        if more:
            s.tune(hn2, blk, [up(t, -12) for t in lower], 72)
        if more == 2:
            s.tune(st, blk, [up(t, 12) for t in lower[:3]], 72)
        if more:
            s.tune(pc, blk + 3, [up(upper[3], 12)], 54)
        for b, beats in ((blk, (0, 1.5, 3) if more == 2 else (0, 1.5)), (blk + 1, (0,)), (blk + 2, (0, 1.5)),
                         (blk + 3, (0, 2))):
            for beat in beats:
                hit(s, hi_, b, beat, 110 if beat == 0 else 100, timpani=tp if beat == 0 else None)
        for b in (blk + 1, blk + 3):
            r = chord(s.sym_at(b, 0)).root
            s.line(tp, b, '%s:1.5:.5 %s:3:.5' % (nm(in_range(r, 33, 44)), nm(in_range(r + 7, 31, 42))), 80)
    timp_roll(s, tp, W2 + 3, 2, 2, 'C2', 20, 92)
    # the horn's tune over the descent to C, the high strings falling a step a bar, the second horn under; the
    # second time the strings on the tune an octave up and the brass on the falling line
    for P, second in ((T1, False), (T3, True)):
        for blk in (P, P + 4):
            s.crash(blk, 0, 100)
            for b in range(blk, blk + 4):
                hit(s, hi_, b, 0, 104 if b == blk else 98, timpani=tp)
                if b % 2 == 1:
                    hit(s, hi_, b, 2.5, 96)
        sung(s, hn, P, SC_TUNE, 90 if not second else 92, e0=80, e1=127, e2=104)
        sung(s, hn2, P, SC_MID, 70)
        if not second:
            s.tune(st, P, SC_HIGH, 66)
        else:
            s.tune(st, P, [up(t, 12) for t in SC_TUNE], 74)
            sung(s, br, P, [up(t, -12) for t in SC_HIGH], 80, e0=70, e1=124, e2=96)
        run(s, pc, P + 7, 2, scale_run(72, 84, (0, 1, 3, 5, 7, 8, 10)), 0.25, 58, 2)   # C phrygian, up
        timp_roll(s, tp, P + 7, 0, 4, 'C2', 12, 96)
    toms(s, T2 + 3, 3, 84)
    toms(s, T4 + 3, 2, 86, n=8)
    # the blades drawn out (2026-11-17, the user, of the riff a half step up and the cut after it: at 0:40 "This
    # part is a bit unpleasant and sounds like a kid mashing around on a piano", at 0:50 "better here again"): from
    # one C the brass climbs to G and the horn falls to the B under it, over A flat, B flat, G7 and home to C
    # minor (the seventh falling to E flat, the leading note rising to C); the strings with the brass (then
    # between them), the second horn under the horn, the hits on the 3+3+2 every other bar
    s.crash(X, 0, 104)
    s.tune(br, X, SC_OPEN_UP, 92)
    s.tune(st, X, SC_OPEN_UP[:2] + ['B4:0:3 D5:3:1', 'C5:0:4'], 72)          # (then the G7's middle)
    s.tune(hn, X, SC_OPEN_DOWN, 86)
    s.tune(hn2, X, [up(t, -12) for t in SC_OPEN_DOWN], 70)
    for b, beats in ((X, ACC332), (X + 1, (0,)), (X + 2, ACC332), (X + 3, (0, 1.5))):
        for beat in beats:
            hit(s, hi_, b, beat, 110 if beat == 0 else 100, timpani=tp if beat == 0 else None)
    for b in (X + 1, X + 3):
        r = chord(s.sym_at(b, 0)).root
        s.line(tp, b, '%s:1.5:.5 %s:3:.5' % (nm(in_range(r, 33, 44)), nm(in_range(r + 7, 31, 42))), 80)
    toms(s, X + 3, 3, 84)
    # the climb: the brass a step a bar (D flat, E flat), a pickup into each, the strings an octave over it, the
    # horns under; a roll into the stop on F minor on the 3+3+2 (consonant: the old stop's stacked C, G, D flat
    # gone), then D flat sighing to C, toms into the riff for everyone
    s.crash(Y, 0, 106)
    s.tune(br, Y, SC_CLIMB, 90)
    s.tune(st, Y, [up(t, 12) for t in SC_CLIMB], 76)
    s.tune(hn, Y, ['Db4:0:4', 'Eb4:0:4'], 82)
    s.tune(hn2, Y, ['Ab3:0:4', 'Bb3:0:4'], 72)
    for b in (Y, Y + 1):
        for beat in ACC332:
            hit(s, hi_, b, beat, 110 if beat == 0 else 102, timpani=tp if beat == 0 else None)
    timp_roll(s, tp, Y + 1, 2, 2, 'C2', 24, 100)
    for beat in ACC332:
        s.play(br, Y + 2, beat, .5, ['F4', 'Ab4', 'C5'], 96)
        s.play(hn, Y + 2, beat, .5, ['F3', 'C4'], 86)
        s.play(mo, Y + 2, beat, .4, ['F3', 'F4'], 84)                    # the motor with them
        hit(s, hi_, Y + 2, beat, 116, timpani=tp)
    s.crash(Y + 2, 0, 110)
    hit(s, hi_, Y + 3, 0, 108, timpani=tp)
    s.tune(br, Y + 3, ['Db5:0:2 C5:2:1.9'], 88)
    s.tune(hn, Y + 3, ['F4:0:2 E4:2:1.9'], 80)
    toms(s, Y + 3, 2, 86, n=8)
    s.end(n)
    return s


# --- BATTLE7: "Closing In" (C minor, 140) ------------------------------------------
# Check Six's sounds and its dread on the downbeats, the other way up: the bass CLIMBS a half step a bar, each new
# note held by the low horn and the low strings and struck by the timpani (C, D flat, D, E flat, then E, F, F sharp
# to G), under a riff that holds C on every downbeat, so the bass closes in on it.  The riff (the riff's own strings
# in octaves) is its C and four dotted eighths, 4+3+3+3+3 in sixteenths, across the beats into the next downbeat,
# the low strings' straight eighths under it and the motor's top notes with it; the hits on one and on the and of
# three (5+3 in eighths).  The horn's tune over the climb in long notes: a call up a fourth, the same call a half
# step higher with the bass, down to D; again, a fourth higher to its top, and down to the leading note.  Then the
# climb goes on past G into A flat minor (the riff a major third down, the brass leaning up with the bass) and comes
# home up whole steps; the strides down the circle of fifths, the call passed between the brass and the horn, each
# holding its goal while the other calls; last the riff with the tune over it on the strings and the horn.

CI_A1 = ['Cm', 'Dbmaj7', 'Dm7b5', 'Cm/Eb G']
CI_A2 = ['C7/E', 'Fm', 'F#dim7', 'Gsus4 G']
CI_D1 = ['Abm', 'A', 'Bbm7b5', 'Abm/B G']
CI_D2 = ['Db', 'Eb', 'Fm', 'Gsus4 G']
CI_S1 = ['Abmaj7', 'Dm7b5', 'G7', 'Cm']
CI_S2 = ['Fm7', 'Bb7', 'Ebmaj7', 'Ab G']
CI_RIFF = ['C5:0:.75 G4:1:.75 D5:1.75:.75 Eb5:2.5:.75 G4:3.25:.75',      # (the top reaching up into the and of
           'C5:0:.75 Ab4:1:.75 Eb5:1.75:.75 F5:2.5:.75 Ab4:3.25:.75',     # three, a chord's note under the hit;
           'C5:0:.75 Ab4:1:.75 D5:1.75:.75 F5:2.5:.75 Ab4:3.25:.75',      # over a change to G on three, one
           'C5:0:.75 Eb5:1:.75 G4:1.75:.75 B4:2.5:.75 D5:3.25:.75',       # of G's notes before it)
           'C5:0:.75 G4:1:.75 D5:1.75:.75 E5:2.5:.75 Bb4:3.25:.75',
           'C5:0:.75 Ab4:1:.75 Eb5:1.75:.75 F5:2.5:.75 Ab4:3.25:.75',
           'C5:0:.75 A4:1:.75 Eb5:1.75:.75 F#5:2.5:.75 A4:3.25:.75',
           'C5:0:.75 G4:1:.75 D5:1.75:.75 F5:2.5:.75 B4:3.25:.75']
CI_DARK = [up(t, -4) for t in CI_RIFF[:3]] + ['G#4:0:.75 D#4:1:.75 B4:1.75:.75 D5:2.5:.75 G4:3.25:.75']
CI_CREEP = ['C3:0:4', 'Db3:0:4', 'D3:0:4', 'Eb3:0:2 G2:2:2', 'E3:0:4', 'F3:0:4', 'F#3:0:4', 'G3:0:4']
CI_DEEP = ['Ab3:0:4', 'A3:0:4', 'Bb3:0:4', 'B3:0:2 G3:2:2']                # (the climb on past G)
CI_STAIRS = ['Db3:0:4', 'Eb3:0:4', 'F3:0:4', 'G3:0:4']
CI_STRIDES = ['Ab3:0:4', 'D3:0:4', 'G3:0:4', 'C3:0:4', 'F3:0:4', 'Bb2:0:4', 'Eb3:0:4', 'Ab2:0:2 G2:2:2']
CI_TUNE = ['G4:0:1 C5:1:3', 'Ab4:0:1 Db5:1:3', 'F5:0:3 D5:3:1', 'Eb5:0:2 D5:2:2',
           'G4:0:1 C5:1:3', 'C5:0:1 F5:1:3', 'Eb5:0:3 D5:3:1', 'C5:0:2 B4:2:2']
CI_CALL = (['Eb5:0:1 Ab5:1:7', '', 'D5:0:1 G5:1:7', '', 'C5:0:1 F5:1:7', '', 'Bb4:0:1 Eb5:1:5', 'D5:2:2'],
           ['C5:0:4', 'Ab4:0:1 D5:1:7', '', 'G4:0:1 C5:1:3', 'C5:0:4', 'F4:0:1 Bb4:1:7', '', 'C5:0:2 B4:2:2'])
CI_LOW = [(0, .35, 'r'), (.5, .35, 'r'), (1, .35, 'r'), (1.5, .35, '8'), (2, .35, 'r'), (2.5, .35, 'r'),
          (3, .35, '8'), (3.5, .35, 'r')]                   # (straight eighths: the riff's dotted ones across them)
CI_PULSE = (0, 1, 1.75, 2.5, 3.25)                  # the riff's own rhythm (4+3+3+3+3 in sixteenths)
CI_TURN = [0, 2, 1, 2, 3, 1, 2, 3, 1, 2, 3, 1, 2, 3, 1, 2]     # (the top note on the riff's dotted eighths)


def cross(s, ch, bars, vel, more=False):
    """the hits on the riff's 5+3 (in eighths): the kick and an orchestra hit on one, a hit and the snare on the and
    of three; more: the kick on the last sixteenth of beat two too (the riff's second dotted eighth), with a hit
    every other bar"""
    for b in bars:
        hit(s, ch, b, 0, vel)
        if more and (b - bars.start) % 2:
            hit(s, ch, b, 1.75, vel - 8, kick=100)
        elif more:
            s.play(9, b, 1.75, 0.2, DRUM['kick'], 100, 3)
        s.play(9, b, 2.5, 0.2, DRUM['snare'], 92, 3)
        s.play(ch, b, 2.5, 0.4, in_range(chord(s.sym_at(b, 2.5)).root, 60, 71), vel - 10, 3)


def closing_in():
    """BATTLE7, "Closing In": the bass climbing under a riff that holds C"""
    form = ['G'] + CI_A1 + CI_A2 + CI_D1 + CI_D2 + CI_A1 + CI_A2 + CI_S1 + CI_S2 + CI_A1 + CI_A2
    s = in_time(Piece(BPM, 1515, form, level=0.4))
    riff = 12                                       # (the riff's own strings, spiccato in octaves: Check Six's)
    s.setup(riff, 44, 110)
    ch = begin(s, extra=(riff,))
    mo, hi_, lo, br, hn, hn2, tp, st, bs = (ch[k] for k in ('motor', 'hit', 'low', 'brass', 'horn', 'horn2',
                                                            'timpani', 'strings', 'bass'))
    n = len(form)                                   # 1 + 40 = 41 bars, 70.3 s
    A1, A2, D1, D2, T1, T2, S1, S2, Q1, Q2 = (1 + 4 * k for k in range(10))
    s.loop_at(1)
    s.sections(1, n)
    fresh(s, range(1, n), (hn, hn2, br, bs, st))
    motor(s, mo, range(0, n), CI_TURN, 58)
    s.bass(lo, range(0, n), CI_LOW, 82, lo=36, hi=47)
    timp_roll(s, tp, 0, 0, 4, 'G1', 10, 92)         # (the intro: on G, into the first C)

    def downbeats(blk, notes, horn_vel=84, strings=True, timp=(31, 44)):
        """every bar's new bass note landing: held by the low horn (and the low strings), struck by the timpani"""
        landing(s, hn2, blk, notes, horn_vel)
        if strings:
            landing(s, bs, blk, [up(t, -12) for t in notes], 78)
        for i, t in enumerate(notes):
            for tok in t.split():
                name, beat, _dur = tok.split(':')
                s.play(tp, blk + i, float(beat), .6, in_range(p(name), *timp), 92 if beat == '0' else 82)

    def pulses(blk, vel=84):
        """the riff's strings in the riff's rhythm on each chord's bass in octaves, short"""
        for b in range(blk, blk + 4):
            for beat in CI_PULSE:
                q = in_range(chord(s.sym_at(b, beat)).bass, 48, 59)
                s.play(riff, b, beat, .2, [q, q + 12], vel + (8 if beat in (0, 2.5) else 0), 3)

    # the riff over the climb, the hits on its 5+3.  The last time the tune over it, on the strings with the horn
    # an octave under (the low strings' voices to the tune)
    for blk, text, vel in ((A1, CI_RIFF[:4], 92), (A2, CI_RIFF[4:], 92), (Q1, CI_RIFF[:4], 94),
                           (Q2, CI_RIFF[4:], 92)):
        s.crash(blk, 0, 100 if blk in (A1, Q1) else 94)
        s.tune(riff, blk, text, vel)
        s.tune(riff, blk, [up(t, -12) for t in text], vel - 8)
        downbeats(blk, CI_CREEP[:4] if blk in (A1, Q1) else CI_CREEP[4:], strings=blk < Q1)
        cross(s, hi_, range(blk, blk + 4), 110 if blk < Q1 else 112)
    toms(s, A2 + 3, 3, 80)
    sung(s, st, Q1, [up(t, 12) for t in CI_TUNE], 90, e0=90, e1=127, e2=104)
    sung(s, hn, Q1, CI_TUNE, 86, e0=90, e1=127, e2=104)
    timp_roll(s, tp, Q2 + 3, 2, 2, 'G1', 20, 100)
    toms(s, Q2 + 3, 3, 86)
    # the climb on past G into A flat minor: the riff a major third down, the brass over it leaning up a half step
    # with the bass (E flat, E, E, E flat) and down to D; the last bar's A flat minor over B turning to G
    s.crash(D1, 0, 108)
    s.tune(riff, D1, CI_DARK, 90)
    s.tune(riff, D1, [up(t, -12) for t in CI_DARK], 82)
    sung(s, br, D1, ['Eb5:0:4', 'E5:0:4', 'E5:0:4', 'Eb5:0:2 D5:2:2'], 86, e0=70, e1=127, e2=90)
    downbeats(D1, CI_DEEP, timp=(35, 47))
    cross(s, hi_, range(D1, D1 + 4), 110)
    # home up whole steps (D flat, E flat, F, G): the brass and the horn climbing in thirds, the riff's strings in
    # its rhythm, the hits on its accents, a roll and the toms
    s.crash(D2, 0, 104)
    sung(s, br, D2, ['F5:0:4', 'G5:0:4', 'Ab5:0:4', 'G5:0:4'], 90, e0=80, e1=127, e2=110)
    sung(s, hn, D2, ['Db5:0:4', 'Eb5:0:4', 'F5:0:4', 'C5:0:2 B4:2:2'], 82, e0=80, e1=127, e2=110)
    downbeats(D2, CI_STAIRS, 82, strings=False)
    pulses(D2)
    cross(s, hi_, range(D2, D2 + 4), 110, more=True)
    timp_roll(s, tp, D2 + 3, 0, 4, 'G1', 12, 100)
    toms(s, D2 + 3, 2, 86, n=8)
    # the horn's tune over the climb, the brass holding the chords, the riff's strings driving in its rhythm
    for blk in (T1, T2):
        s.crash(blk, 0, 104)
        cross(s, hi_, range(blk, blk + 4), 108)
        pulses(blk)
    sung(s, hn, T1, CI_TUNE, 92, e0=80, e1=127, e2=110)
    block_pad(s, br, range(T1, T1 + 8), 57, 70, 2, 60)      # (two: the riff's strings' voices)
    downbeats(T1, CI_CREEP[:4], 80, strings=False)
    downbeats(T2, CI_CREEP[4:], 80, strings=False)
    timp_roll(s, tp, T2 + 3, 2, 2, 'G1', 20, 98)
    toms(s, T2 + 3, 3, 82)
    # the strides down the circle of fifths: the call (up a fourth) passed between the brass and the horn, each
    # holding its goal while the other calls; the riff's strings in eighths, more hits the second time, into the end
    for blk in (S1, S2):
        s.crash(blk, 0, 104)
        cross(s, hi_, range(blk, blk + 4), 110, more=blk == S2)
        for b in range(blk, blk + 4):
            for k in range(8):
                q = in_range(chord(s.sym_at(b, k * .5)).root, 60, 71)
                s.play(riff, b, k * .5, .3, [q, q + 12], 68 + (8 if k % 2 == 0 else 0), 3)
    sung(s, br, S1, CI_CALL[0], 90, e0=80, e1=127, e2=104)
    sung(s, hn, S1, CI_CALL[1], 84, e0=80, e1=127, e2=104)
    downbeats(S1, CI_STRIDES[:4], 82, strings=False)
    downbeats(S2, CI_STRIDES[4:], 82, strings=False)
    timp_roll(s, tp, S2 + 3, 0, 4, 'G1', 12, 100)
    toms(s, S2 + 3, 2, 86, n=8)
    s.end(n)
    return s


SONGS = {'BATTLE4.MID': intercept, 'BATTLE5.MID': check_six, 'BATTLE6.MID': scissors, 'BATTLE7.MID': closing_in}
