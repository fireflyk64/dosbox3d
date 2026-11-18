"""Three more panics for the third draft's orchestra (CC0, ours): PANIC1B and PANIC2B, the alternatives the score
picks between at random with Hull Breach and Systems Failing, and PANIC3B, which takes turns with Abandon Ship.

The user (2026-11-17, in the soundtrack review), of Hull Breach (the hull under 60%): "We need one more of this
level of panic to randomly select---that one is a bit calmer in general but still has the tension"; of Systems
Failing (under 35%): "Another systems failing one--- a bit more focused for the alternative so the player has time
to make it right"; of Abandon Ship (under 15%; "keep, more like this"): "we probably need a similar variant for this
one so players arent stuck in this song forever".

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
    PANIC3B  "Last Stand"      C, 140: Abandon Ship's way (the fight quoted note for note, two-bar bursts of panic
                               between) with the newer battles' blocks (Scissors, Intercept, Check Six) and bursts of
                               its own in the half step's colours, not the tritone's: the hull giving way (the bass
                               falling by half steps under a held C), the breach (D flat over the C pedal, A flat
                               over G), the ship listing (the orchestra on the dotted quarters), the fight back (the
                               bass climbing), the last stand's cry; bracing on G into the loop, at 140 throughout

PANIC1B and PANIC2B at the battles' 140 in C, a bar's intro (the damage, the alarm) played once, then the loop, a
"sect" marker at every block of four bars; each block starts on its downbeat with the motor and a hit and leaves
nothing hanging over the next.  PANIC3B is all loop, a "sect" at each quote and burst but the last.  Their drums are
hits, rolls and toms, no kit's beat.
"""
from compose import chord, Voicer
from orch import NAMES, Piece, up, ACC332, orchestra, in_range, pedal332, hit, held, sung, excerpt, restore, in_time
from panics import motor16, low8, swelled, trill
from battles2 import fresh, toms
from battles import block_pad
import battles2

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


# --- PANIC3B: "Last Stand" (C, 140): the hull under 15%, Abandon Ship's other half ---------------------------------
# The score plays the two in turn, a pass each, splicing on the bar line from each one's loop marker.  So this keeps
# Abandon Ship's way and its pace (the fight quoted note for note, a two-bar burst between quotes, 36 bars) but is
# another piece: the newer battles quoted (Scissors' blades for everyone, Intercept's call for everyone and its build,
# Check Six's turn into E flat minor and its sighs, Scissors' cut), and bursts of its own in the half step's colours
# where Abandon Ship's are the tritone's, scored heavy and low (the horns, the brass, the timpani; no snare rolls, the
# piccolo resting): the hull giving way (C minor struck, the bass falling by half steps a beat to G under a C held
# high); the breach (the chord a half step over the pedal, the horn ripping up to it); the ship listing (the
# orchestra on the dotted quarters, into E flat minor); the fight back (the bass climbing by half steps); the cry
# (the brass and the horn in octaves, long notes); and bracing on G (D flat, the Neapolitan, to the dominant, rolling
# and swelling), at 140 throughout.  The joins: from Abandon Ship's end (G flat at about 200, a snare roll) its
# first bar is C minor struck by everyone at 140, as Abandon Ship's own loop does it; its own end (G, the timpani
# and the swell at full, the toms down) falls on C minor's downbeat, Abandon Ship's first bar or its own.

_QUOTED2 = {}


def quoted2(name):
    """the newer battle pieces, made once (Last Stand quotes them)"""
    if name not in _QUOTED2:
        make = {'intercept': battles2.intercept, 'checksix': battles2.check_six, 'scissors': battles2.scissors}[name]
        _QUOTED2[name] = make()
    return _QUOTED2[name]


def ls_sinking(s, c, b0, _arg):
    """the hull giving way: C minor struck by everyone, then the bass falling by half steps a beat (C, B, B flat, A,
    A flat: the old lament at four times its pace) to G, the low strings on it, the brass's chords sinking with it,
    under a C held high by the horn and the strings that falls to B on the G; the timpani on each step, then
    rolling"""
    fall = [48, 47, 46, 45, 44, 44, 43, 43]                 # (C3 down to G2, a beat each)
    s.crash(b0, 0, 110)
    for k, b in enumerate((b0, b0 + 1)):
        motor16(s, c['motor'], b, 64 + 4 * k, lo=43)
        for e in range(8):
            q = fall[k * 4 + e // 2]
            s.play(c['low'], b, e * .5, .45, q, 84 + (8 if e % 2 == 0 else 0))
    s.pad(c['brass'], range(b0, b0 + 2), 55, 67, 2, 84)
    for ch, octave, vel in ((c['horn'], 0, 94), (c['strings'], 12, 74)):
        held(s, ch, b0, 0, 5.9, 72 + octave, vel, e0=110, e1=127, e2=100)
        held(s, ch, b0 + 1, 2, 1.9, 71 + octave, vel + 2, e0=100, e1=127, e2=116)
    hit(s, c['hit'], b0, 0, 114)
    for beat, q in enumerate(('C2', 'B1', 'Bb1', 'A1')):
        s.play(c['timpani'], b0, beat, .5, q, 94 if beat == 0 else 84)
    s.play(c['timpani'], b0 + 1, 0, .5, 'Ab1', 90)
    hit(s, c['hit'], b0 + 1, 0, 104)
    hit(s, c['hit'], b0 + 1, 2, 108)
    s.roll(c['timpani'], b0 + 1, 2, 2, 'G1', 50, 110, step=0.125)


def ls_rising(s, c, b0, _arg):
    """the fight back: sinking turned over, the bass climbing by half steps a beat (C, C sharp, D, E flat) to F and
    G, the low strings on it, the brass's chords climbing with it, under a G held high by the horn and the strings
    that sighs to A flat and back; the timpani on each step, then rolling"""
    rise = [36, 37, 38, 39, 41, 41, 43, 43]                 # (C2 up to G2)
    s.crash(b0, 0, 106)
    for k, b in enumerate((b0, b0 + 1)):
        motor16(s, c['motor'], b, 64 + 4 * k)
        for e in range(8):
            q = rise[k * 4 + e // 2]
            s.play(c['low'], b, e * .5, .45, q, 84 + (8 if e % 2 == 0 else 0))
    s.pad(c['brass'], range(b0, b0 + 2), 55, 67, 2, 84)
    sung(s, c['horn'], b0, ['G4:0:4', 'Ab4:0:2 G4:2:1.9'], 94, e0=100, e1=127, e2=110)
    sung(s, c['strings'], b0, ['G5:0:4', 'Ab5:0:2 G5:2:1.9'], 72, e0=96, e1=127, e2=110)
    for beat in (0, 2):
        hit(s, c['hit'], b0, beat, 112 if beat == 0 else 104)
    for beat, q in enumerate(('C2', 'C#2', 'D2', 'Eb2')):
        s.play(c['timpani'], b0, beat, .5, q, 92 if beat == 0 else 84)
    s.play(c['timpani'], b0 + 1, 0, .5, 'F2', 90)
    hit(s, c['hit'], b0 + 1, 0, 106)
    hit(s, c['hit'], b0 + 1, 1.5, 102)
    s.roll(c['timpani'], b0 + 1, 2, 2, 'G2', 50, 110, step=0.125)


def ls_breach(s, c, b0, root):
    """the breach: the major chord a half step over the pedal (D flat over C, A flat over G), the horn ripping up to
    it (to the third) and the brass and the strings holding it, swelling, over the motor and the low strings on the
    pedal; the hits and the timpani on the pedal on the 3+3+2, the toms down into the fight's next downbeat, where
    the half step falls home"""
    top = root + 1
    s.crash(b0, 0, 112)
    for k, b in enumerate((b0, b0 + 1)):
        motor16(s, c['motor'], b, 66 + 4 * k, pedal=root)
        low8(s, c['low'], b, 82 + 2 * k, pedal=root)
        for beat in ACC332:
            s.play(9, b, beat, .2, 36, 114 if beat == 0 else 104, 3)
            s.play(c['hit'], b, beat, .4, in_range(root, 60, 71), 112 if beat == 0 else 102, 3)
            s.play(c['timpani'], b, beat, .5, in_range(root, 33, 44), 94 if beat == 0 else 84)
    r = in_range(top, 43, 54)
    for i, q in enumerate((r, r + 4, r + 7, r + 12)):        # the rip, in thirty-seconds
        s.play(c['horn'], b0, i * .125, .12, q, 84 + 4 * i, 2)
    held(s, c['horn'], b0, .5, 7.4, r + 16, 98, e0=110, e1=127, e2=127)
    swelled(s, c['brass'], b0, 0, 7.9, [r + 12, r + 19], 90, e0=90, e1=118, e2=127)
    swelled(s, c['strings'], b0, 0, 7.9, [r + 24], 74, e0=80, e1=116, e2=127)
    toms(s, b0 + 1, 3, 86)


def ls_list(s, c, b0, _arg):
    """the ship listing: the orchestra on the dotted quarters across both bars (3+3+3+3+3+1 eighths), held, the
    hits and the timpani with them, the motor's sixteenths keeping the time under it; the brass in a long line over
    it (E flat held, then D: into E flat minor), the horn under that"""
    strokes = (0, 1.5, 3, 4.5, 6, 7.5)
    s.crash(b0, 0, 106)
    for k, b in enumerate((b0, b0 + 1)):
        acc = [int(x * 4) - 16 * k for x in strokes if 4 * k <= x < 4 * k + 4]
        motor16(s, c['motor'], b, 62 + 4 * k, accents=acc)
    v = Voicer(55, 67, 2)
    for i, x in enumerate(strokes):
        b, beat = b0 + int(x // 4), x % 4
        h = chord(s.sym_at(b, beat))
        dur = min(1.45, 7.95 - x)
        q = in_range(h.bass, 36, 47)
        s.play(c['low'], b, beat, dur, q, 94 if i in (0, 3) else 88)
        s.play(c['horn2'], b, beat, dur, v.next(h), 84 if i in (0, 3) else 78)
        s.play(c['timpani'], b, beat, .5, in_range(h.bass, 33, 44), 92 if i in (0, 3) else 84)
        hit(s, c['hit'], b, beat, 112 if i in (0, 3) else 104)
    sung(s, c['brass'], b0, ['Eb5:0:4', 'Eb5:0:2 D5:2:1.9'], 92, e0=96, e1=127, e2=110)
    sung(s, c['horn'], b0, ['G4:0:4', 'Ab4:0:2 F4:2:1.9'], 88, e0=96, e1=127, e2=110)


def ls_cry(s, c, b0, _arg):
    """the last stand's cry: the brass and the horn an octave under it (G, C, a long E flat; F, E flat, a long D)
    over the motor and the low strings on the chords, the strings holding them; the hits on the 3+3+2, the timpani
    in eighths on the bass, swelling"""
    cry = ['G4:0:.5 C5:.5:.5 Eb5:1:3', 'F5:0:1.5 Eb5:1.5:.5 D5:2:1.9']
    s.crash(b0, 0, 108)
    sung(s, c['brass'], b0, cry, 94, e0=100, e1=127, e2=116)
    s.tune(c['horn'], b0, [up(t, -12) for t in cry], 90)
    s.pad(c['strings'], range(b0, b0 + 2), 63, 76, 2, 66)
    for k, b in enumerate((b0, b0 + 1)):
        motor16(s, c['motor'], b, 64 + 4 * k)
        low8(s, c['low'], b, 82 + 2 * k)
        for beat in ACC332:
            hit(s, c['hit'], b, beat, 112 if beat == 0 else 102)
        for e in range(8):
            q = in_range(chord(s.sym_at(b, e * .5)).bass, 31, 42)
            s.play(c['timpani'], b, e * .5, .3, q, 70 + 2 * (8 * k + e) + (8 if e % 2 == 0 else 0))


def ls_brace(s, c, b0, _arg):
    """bracing for it: the dominant two bars over the timpani rolling on G from nothing, the motor and the low
    strings on the G pedal, the second horn holding it; the brass, the horn an octave under and the strings an
    octave over climbing a minor third each half bar (D, F, A flat, B: the diminished seventh over G), swelling to
    full; the hits on the second bar's 3+3+2, the toms down into C minor's downbeat (Abandon Ship's first bar, or
    this one's)"""
    s.crash(b0, 0, 96)
    hit(s, c['hit'], b0, 0, 104)
    s.roll(c['timpani'], b0, 0, 8, 'G1', 30, 118, step=0.125)
    for k, b in enumerate((b0, b0 + 1)):
        motor16(s, c['motor'], b, 60 + 8 * k, pedal=7)
        low8(s, c['low'], b, 76 + 8 * k, pedal=7)
    for beat, v in zip(ACC332, (106, 110, 114)):
        hit(s, c['hit'], b0 + 1, beat, v)
    line = ['D5:0:2 F5:2:2', 'Ab5:0:2 B5:2:1.9']
    for ch, semis, vel in ((c['brass'], -12, 92), (c['horn'], -24, 88), (c['strings'], 0, 70)):
        s.tune(ch, b0, [up(t, semis) for t in line], vel)
        s.ramp(ch, b0, 2, 56, 127)
    s.play(c['horn2'], b0, 0, 7.9, 'G3', 84)
    s.ramp(c['horn2'], b0, 2, 60, 127)
    toms(s, b0 + 1, 3, 88)


LS_BURSTS = {'sinking': (lambda _a: ['Cm G/B Cm/Bb F/A', 'Fm/Ab G'], ls_sinking),
             'rising': (lambda _a: ['Cm C#dim7 G/D Cm/Eb', 'Fm G'], ls_rising),
             'breach': (lambda r: ['%s/%s' % (NAMES[(r + 1) % 12], NAMES[r % 12])] * 2, ls_breach),
             'list': (lambda _a: ['Cm', 'Ab Bb'], ls_list),
             'cry': (lambda _a: ['Cm', 'Ab G'], ls_cry),
             'brace': (lambda _a: ['G', 'G7b9'], ls_brace)}
LS_PLAN = [('sinking', 0), ('fight', 'scissors', 30, 4),         # the blades for everyone (ends on D flat)
           ('breach', 0), ('fight', 'intercept', 26, 4),        # the call and answer for everyone
           ('list', 0), ('fight', 'checksix', 25, 4),           # the riff in E flat minor, the sighs (ends on B flat)
           ('rising', 0), ('fight', 'intercept', 22, 4),        # the build (into the breach on G)
           ('breach', 7), ('fight', 'checksix', 9, 4),          # the brass's sighs, the riff replying
           ('cry', 0), ('fight', 'scissors', 26, 2),            # the cut's two blades (ends on D flat)
           ('brace', 0)]


def last_stand():
    form = []
    for step in LS_PLAN:
        form += ['Cm'] * step[3] if step[0] == 'fight' else LS_BURSTS[step[0]][0](step[1])
    s = in_time(Piece(BATTLE_BPM, 2518, form, level=-0.6))
    parts = orchestra(s)
    for c in parts.values():
        s.cc(c, 0, 0, 11, 127)
    n = len(form)                                   # 36 bars, 61.7 s
    s.loop_at(0)
    b = 0
    for step in LS_PLAN:
        if b < n - 2:                               # (a "sect" at each step but the brace: 12)
            s.sect(b)
        if step[0] == 'fight':
            excerpt(s, quoted2(step[1]), step[2], step[3], b)
            b += step[3]
        else:
            restore(s, b)
            LS_BURSTS[step[0]][1](s, parts, b, step[1])
            b += 2
    s.end(n)
    return s


SONGS = {'PANIC1B.MID': damage_report, 'PANIC2B.MID': reroute_power, 'PANIC3B.MID': last_stand}
