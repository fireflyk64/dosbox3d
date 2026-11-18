"""Two more panics for the third draft's orchestra (CC0, ours): PANIC1B and PANIC2B, the alternatives the score
picks between at random with Hull Breach and Systems Failing.

The user (2026-11-17, in the soundtrack review), of Hull Breach (the hull under 60%): "We need one more of this
level of panic to randomly select---that one is a bit calmer in general but still has the tension"; of Systems
Failing (under 35%): "Another systems failing one--- a bit more focused for the alternative so the player has time
to make it right".

    PANIC1B  "Damage Report"   C, 140: the fight going on under the damage, calmer than Hull Breach: the motor on its
                               C pedal, the low strings only on the 3+3+2, a heartbeat on the timpani, the horn's
                               tune (a sigh on G, the D flat's bite, G7 over the pedal); the strings' line over a
                               breathing horn drone; Contact's call slowed to half speed on the low horn, then the
                               brass; the tune on the brass; one little spurt of alarm an eight-bar phrase
    PANIC2B  "Reroute Power"   C, 140: focused, not wild: a steady pulse (the motor and the low strings on the
                               chords' bass, the timpani on one and three) under a ground of four bars (C minor,
                               A flat, F minor or D flat, G) and one clear line, step, step, hold: the horn, then the
                               brass an octave up, the strings' line in F minor, all of it together; the alarms short,
                               in time, once a phrase

Both at the battles' 140 in C, a bar's intro (the damage, the alarm) played once, then the loop, a "sect" marker
at every block of four bars; each block starts on its downbeat with the motor and a hit and leaves nothing hanging
over the next.  Their drums are hits, rolls and toms, no kit's beat.
"""
from compose import chord
from orch import Piece, up, ACC332, orchestra, in_range, pedal332, hit, sung
from panics import motor16, low8, swelled, trill
from battles2 import fresh, toms
from battles import block_pad

BATTLE_BPM = 140


# --- PANIC1B: "Damage Report" (C, 140): the hull under 60%, the calmer one ------------------------------------------
# Hull Breach's fight is Contact's drive with the damage screaming over it.  Here the fight goes on with the damage
# under it: the motor on its C pedal but softer, the low strings only on the 3+3+2, the timpani a heartbeat, the
# second horn's drone breathing (a fifth, a cluster where the D flat comes); the tension is in the harmony (D flat
# and G7 over the C pedal, C minor with B in it) and in a tune that sighs.  A bar's damage first (struck and dying
# away), then eight blocks: the horn's tune; the strings' line; Contact's call slowed to half speed, on the low horn
# and then the brass; the tune on the brass, and into the loop.  A little spurt of alarm once a phrase.

DR_P0 = ['Cm', 'Ab/C', 'Cm', 'Db/C']
DR_P1 = ['Cm', 'Ab/C', 'Fm/C', 'G7/C']
DR_S0 = ['Ab/C', 'Fm/C', 'Db/C', 'Cm']
DR_S1 = ['Ab/C', 'Fm/C', 'Db/C', 'G7/C']
DR_K = ['Cmmaj7', 'Cm', 'Cmmaj7', 'Cm']
DR_C = ['Cm', 'Ab/C', 'Bb/C', 'Gm/C']
DR_TUNE = ['G4:0:1.5 Ab4:1.5:.5 G4:2:2', 'C5:0:3 Bb4:3:.5 Ab4:3.5:.5', 'G4:0:4', 'F4:0:1.5 Ab4:1.5:.5 Db5:2:2',
           'C5:0:1.5 D5:1.5:.5 Eb5:2:2', 'Eb5:0:1.5 F5:1.5:.5 Eb5:2:1 C5:3:1', 'Ab4:0:1.5 C5:1.5:.5 F5:2:2',
           'F5:0:2 D5:2:1 B4:3:1']
DR_LINE = ['C5:0:2 Eb5:2:2', 'F5:0:3 Ab5:3:1', 'Ab5:0:2 F5:2:2', 'G5:0:4',
           'C6:0:3 Bb5:3:1', 'Ab5:0:2 F5:2:2', 'F5:0:2 Db5:2:2', 'D5:0:2 B4:2:2']
DR_SLOW = ['C4:0:3 G4:3:1', 'G4:0:4', 'Ab4:0:2 G4:2:2', 'F4:0:2 Eb4:2:2',      # Contact's call (panics.CT_CALL)
           'G4:0:3 C5:3:1', 'C5:0:4', 'D5:0:2 C5:2:2', 'Bb4:0:2 G4:2:2']      # at half speed: its bars 1-2, 5-6


def damage_report():
    form = ['Cm'] + DR_P0 + DR_P1 + DR_S0 + DR_S1 + DR_K + DR_C + DR_P0 + DR_P1
    s = Piece(BATTLE_BPM, 2316, form, level=-0.4)
    ch = orchestra(s)
    hn, mo, hi_, lo, br, hn2, pc, tp, st = (ch[k] for k in ('horn', 'motor', 'hit', 'low', 'brass', 'horn2',
                                                          'piccolo', 'timpani', 'strings'))
    for c in ch.values():
        s.cc(c, 0, 0, 11, 127)
    n = len(form)                                   # 1 + 32 = 33 bars, 56.6 s
    B = [1 + 4 * k for k in range(8)]
    s.loop_at(1)
    s.sections(1, n)
    fresh(s, range(1, n), (hn, hn2, br, st))
    # the damage, once: struck and dying away, the motor coming in under it
    s.crash(0, 0, 108)
    hit(s, hi_, 0, 0, 112, timpani=tp)
    swelled(s, br, 0, 0, 3.8, ['C4', 'Db4', 'Gb4'], 84, e0=127, e1=84, e2=30)
    swelled(s, hn2, 0, 0, 3.8, ['C3', 'Db3'], 76, e0=127, e1=90, e2=40)
    s.roll(tp, 0, 1, 3, 'C2', 90, 30, step=0.125)
    motor16(s, mo, 0, 50, pedal=0)
    pedal332(s, lo, [0], 74, lo=36)
    for b in range(1, n):
        blk, k = (b - 1) // 4, (b - 1) % 4
        sym = form[b]
        # the fight under it: the motor on the pedal, the low strings on the 3+3+2, then (blocks 5-7) in eighths
        vel = {4: 48, 5: 56 + 2 * k, 6: 60, 7: 60}.get(blk, 54 + k)
        motor16(s, mo, b, vel, pedal=0, figure=(0, 1, 2, 1) if k % 2 == 0 else (2, 1, 0, 1))
        if blk < 5:
            pedal332(s, lo, [b], 74 if blk != 4 else 66, lo=36)
        else:
            low8(s, lo, b, 72, pedal=0)
        # the timpani: a heartbeat (two beats to a beat), then on one and three
        if blk < 5:
            for beat, v in ((0, 80), (.5, 62), (2, 74), (2.5, 58)):
                s.play(tp, b, beat, .3, 'C2', v - (8 if blk == 4 else 0))
        elif k < 3:
            s.line(tp, b, 'C2:0:.5 G1:2:.5', 82)
        # the second horn's drone breathing: a fifth, a cluster where the D flat comes
        if blk != 4:
            cl = ['C3', 'Db3'] if sym.startswith('Db') else ['B2', 'G3'] if sym.startswith('G7') else ['C3', 'G3']
            swelled(s, hn2, b, 0, 3.8, cl, 62 if blk < 5 else 66, e0=40, e1=104, e2=56)
    for blk, v in ((0, 100), (2, 96), (4, 80), (5, 100), (6, 106)):
        s.crash(B[blk], 0, v)
    for blk in range(8):
        hit(s, hi_, B[blk], 0, 104 if blk != 4 else 92, timpani=None if blk < 5 else tp)
    hit(s, hi_, B[5] + 2, 0, 100)
    for beat in ACC332[1:]:
        hit(s, hi_, B[6], beat, 100)
    hit(s, hi_, B[6] + 2, 0, 104)
    hit(s, hi_, B[6] + 2, 1.5, 98)
    hit(s, hi_, B[7] + 2, 0, 100)
    # the horn's tune; the strings' line; the call at half speed (the low horn, then the brass with the horn under);
    # the tune on the brass, the horn an octave under, the strings an octave up at the end
    sung(s, hn, B[0], DR_TUNE, 84, e0=70, e1=124, e2=96)
    sung(s, st, B[2], DR_LINE, 74, e0=60, e1=118, e2=90)
    sung(s, hn, B[4], DR_SLOW[:4], 80, e0=60, e1=120, e2=90)
    for i, q in enumerate(('B5', 'C6', 'B5', 'C6')):              # the strings high, a half step in turn
        s.play(st, B[4] + i, 0, 3.9, q, 50)
    sung(s, br, B[5], DR_SLOW[4:], 84, e0=70, e1=124, e2=100)
    s.tune(hn, B[5], [up(t, -12) for t in DR_SLOW[4:]], 72)
    sung(s, br, B[6], DR_TUNE, 86, e0=80, e1=127, e2=104)
    s.tune(hn, B[6], [up(t, -12) for t in DR_TUNE], 74)
    s.tune(st, B[7], [up(t, 12) for t in DR_TUNE[4:]], 60)
    # the little spurts of alarm, one a phrase: seconds on the brass, the piccolo's trill, a roll
    b = B[1] + 3
    s.play(br, b, 1.5, .4, ['B4', 'C5'], 70)
    s.play(br, b, 3, .4, ['B4', 'C5'], 74)
    b = B[3] + 3
    trill(s, pc, b, 1, 1, 'C6', 'Db6', 50)
    s.play(br, b, 3, .4, ['C5', 'Db5'], 72)
    s.roll(tp, b, 2, 2, 'C2', 26, 92, step=0.125)
    b = B[5] + 3
    trill(s, pc, b, 2, 1, 'F#6', 'G6', 48)
    s.roll(tp, b, 2, 2, 'G1', 24, 90, step=0.125)
    b = B[7] + 3
    trill(s, pc, b, 0, 1, 'F#6', 'G6', 50)
    s.play(br, b, 1.5, .4, ['B4', 'C5'], 72)
    s.roll(tp, b, 2, 2, 'C2', 26, 96, step=0.125)
    toms(s, b, 3, 80)
    s.end(n)
    return s


# --- PANIC2B: "Reroute Power" (C, 140): the hull under 35%, the focused one -----------------------------------------
# Systems Failing breaks the fight up with wild bursts.  This keeps one pulse and one line, so the player can think:
# the motor and the low strings on the chords' bass, the timpani on one and three, the hits on the blocks'
# downbeats, under a ground of four bars (C minor, A flat, F minor or D flat, G) that the score can lean on; over
# it a line of one figure (two steps and a held note), the horn first, the brass an octave up the second time with
# the second horn under it, then the strings' line in F minor with the brass holding C and D flat under it, then all
# of it together.  The alarms come in time, short, once a phrase: seconds on the off-beats, the piccolo in eighths,
# a roll.  A bar's alarm first, in time, played once.

RP_GA = ['Cm', 'Ab', 'Fm', 'G']
RP_GB = ['Cm', 'Ab', 'Db', 'G']
RP_FA = ['Fm', 'Db', 'Bbm', 'C']
RP_FB = ['Fm', 'Db', 'Gb', 'C']
RP_LINE = ['C5:0:1 D5:1:1 Eb5:2:2', 'Eb5:0:1 D5:1:1 C5:2:2', 'Ab4:0:1 Bb4:1:1 C5:2:2', 'D5:0:3 B4:3:1',
           'Eb5:0:1 F5:1:1 G5:2:2', 'G5:0:1 F5:1:1 Eb5:2:2', 'F5:0:1 Eb5:1:1 Db5:2:2', 'B4:0:2 D5:2:2']
RP_FLINE = ['F5:0:1 G5:1:1 Ab5:2:2', 'Ab5:0:1 G5:1:1 F5:2:2', 'F5:0:1 Eb5:1:1 Db5:2:2', 'E5:0:3 C5:3:1',
            'F5:0:1 G5:1:1 Ab5:2:2', 'Bb5:0:1 Ab5:1:1 F5:2:2', 'Gb5:0:2 Db5:2:1 Bb4:3:1', 'C5:0:2 E5:2:1 G5:3:1']
RP_HOLD = ['C5:0:4', 'Db5:0:4', 'Db5:0:4', 'C5:0:4', 'C5:0:4', 'Db5:0:4', 'Db5:0:4', 'C5:0:4']


def reroute_power():
    form = ['Cm'] + RP_GA + RP_GB + RP_GA + RP_GB + RP_FA + RP_FB + RP_GA + RP_GB
    s = Piece(BATTLE_BPM, 2417, form, level=-0.7)
    ch = orchestra(s)
    hn, mo, hi_, lo, br, hn2, pc, tp, st = (ch[k] for k in ('horn', 'motor', 'hit', 'low', 'brass', 'horn2',
                                                          'piccolo', 'timpani', 'strings'))
    for c in ch.values():
        s.cc(c, 0, 0, 11, 127)
    n = len(form)                                   # 1 + 32 = 33 bars, 56.6 s
    B = [1 + 4 * k for k in range(8)]
    s.loop_at(1)
    s.sections(1, n)
    fresh(s, range(1, n), (hn, hn2, br, st))
    # the alarm, once, in time: seconds on the beats, the motor on its pedal, a roll into the pulse
    s.crash(0, 0, 104)
    hit(s, hi_, 0, 0, 110, timpani=tp)
    for beat in range(4):
        s.play(br, 0, beat, .45, ['C5', 'Db5'], 84 - 3 * beat)
    motor16(s, mo, 0, 58, pedal=0)
    low8(s, lo, 0, 74, pedal=0)
    s.roll(tp, 0, 2, 2, 'G1', 24, 100, step=0.125)
    # the pulse: the motor and the low strings on the chords' bass, the timpani on one and three
    for b in range(1, n):
        blk, k = (b - 1) // 4, (b - 1) % 4
        motor16(s, mo, b, 60 + (4 if blk >= 6 else 0) + (2 if k % 2 else 0), figure=(0, 1, 2, 1))
        low8(s, lo, b, 74 + (4 if blk >= 6 else 0))
        c = chord(s.sym_at(b, 0))
        if not (k == 3 and blk in (3, 5, 7)):
            s.play(tp, b, 0, .5, in_range(c.bass, 33, 44), 84)
            s.play(tp, b, 2, .5, in_range(c.bass + 7, 31, 42), 72)
    for blk in range(8):
        s.crash(B[blk], 0, 100 if blk in (0, 2, 4, 6) else 90)
        hit(s, hi_, B[blk], 0, 108 if blk in (0, 2, 4, 6) else 102)
        if blk in (2, 3):
            for b in range(B[blk] + 1, B[blk] + 4):
                hit(s, hi_, b, 0, 98)
        if blk in (4, 5):
            hit(s, hi_, B[blk] + 2, 0, 100)
        if blk in (6, 7):
            for b, beats in ((B[blk], ACC332[1:]), (B[blk] + 1, (0,)), (B[blk] + 2, (0, 1.5)), (B[blk] + 3, (0,))):
                for beat in beats:
                    hit(s, hi_, b, beat, 104 if beat == 0 else 98)
    # the line: the horn; the brass an octave up, the horn under it and the second horn holding the chords; the
    # strings' line in F minor over the brass's C and D flat; all of it together
    sung(s, hn, B[0], [up(t, -12) for t in RP_LINE], 96, e0=90, e1=127, e2=106)
    s.tune(hn2, B[0], [up(t, -12) for t in RP_LINE], 70)              # (the second horn on it, the first time)
    sung(s, br, B[2], RP_LINE, 86, e0=80, e1=127, e2=104)
    s.tune(hn, B[2], [up(t, -12) for t in RP_LINE], 76)
    block_pad(s, hn2, range(B[2], B[4]), 55, 67, 2, 60)
    sung(s, st, B[4], RP_FLINE, 82, e0=70, e1=124, e2=100)
    sung(s, br, B[4], RP_HOLD, 72, e0=50, e1=110, e2=70)
    s.tune(hn, B[4], [up(t, -12) for t in RP_FLINE], 70)
    block_pad(s, hn2, range(B[4], B[6]), 53, 65, 2, 62)
    sung(s, br, B[6], RP_LINE, 90, e0=90, e1=127, e2=110)
    s.tune(hn, B[6], [up(t, -12) for t in RP_LINE], 80)
    s.tune(st, B[6], [up(t, 12) for t in RP_LINE], 64)
    block_pad(s, hn2, range(B[6], B[6] + 8), 55, 67, 2, 64)
    # the alarms, in time, one a phrase
    b = B[1] + 3
    for beat in (1.5, 2.5, 3.5):
        s.play(br, b, beat, .4, ['B4', 'C5'], 74)
    toms(s, b, 3, 80)
    b = B[3] + 3
    for k in range(4):
        s.play(pc, b, 2 + k * .5, .45, 'G6' if k % 2 == 0 else 'Ab6', 54)
    s.roll(tp, b, 2, 2, 'G1', 24, 98, step=0.125)
    b = B[5] + 3
    s.roll(tp, b, 0, 4, 'C2', 14, 100, step=0.125)
    for beat in (1.5, 2.5, 3.5):
        s.play(br, b, beat, .4, ['C5', 'Db5'], 70)
    toms(s, b, 3, 82)
    b = B[7] + 3
    s.roll(tp, b, 0, 4, 'G1', 14, 102, step=0.125)
    for k in range(4):
        s.play(pc, b, 2 + k * .5, .45, 'G6' if k % 2 == 0 else 'Ab6', 56)
    toms(s, b, 2, 84, n=8)
    s.end(n)
    return s


SONGS = {'PANIC1B.MID': damage_report, 'PANIC2B.MID': reroute_power}
