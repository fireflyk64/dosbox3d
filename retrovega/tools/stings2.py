"""The stings' variations and the won debrief (CC0, ours), on tools/orch.py's orchestra as tools/panics.py writes
the stings.  The user, 2026-11-17, in the soundtrack review: of each splash, "We need another variation of this one
so it stays fresh"; of the carrier's loss, that every loss have a variation; of the won debrief, "more".

    VICTORY4  "Splash Four"    Splash One's sibling made its own (the user: "kinda repetitive for the previous--
                               maybe 2 seconds longer with a surprise or undertone"), A at 116, 6 s: D minor over a
                               low A held under it all (the undertone, the timpani rumbling), the call from the
                               downbeat lifting a half step to the Neapolitan B flat where A was due (the surprise,
                               struck), E7, and A major, the call turning round A into it
    VICTORY5  "Splash Five"    Splash Two's sibling, D at 116: B minor, then G minor's sixth (the minor plagal's
                               B flat) over a bass falling B, B flat, the call's B flat sighing down to A on the
                               D chord, the harp falling after it
    VICTORY6  "Splash Six"     Splash Three's sibling, B flat at 120: C minor and F sus, the timpani rolling into
                               G minor where B flat was due (the deceptive turn, struck), E flat with its sharp
                               eleven, and B flat at last, the call's A rising into it
    LOST2     "Empty Deck"     Carrier Lost's sibling, B flat minor at 46: hollow fifths falling B flat, A flat,
                               G flat, F and back, no third and no beat; the far horn takes up the way home's tune
                               where Carrier Lost broke off (its next two bars), the oboe falls once from far
                               above, a timpani far off under it, once, and the horn begins again and stops
    WIN2      "Stand Down"     the won debrief, E flat at 72, 40 s: harp eighths, the strings held, the horn's
                               tune low, the strings singing it up an octave over the horn, then the oboe over a
                               horn chorale, B flat's sus deceived into C minor and the minor fourth (A flat
                               minor) turning it home: relief and a quiet pride, not a fanfare

The stings play once over the flight's song (src/music.c music_sting); LOST2 loops as LOST does (its last bar the
strings' alone), and WIN2 loops from its start (its last bar's A flat minor leads into its first bar's E flat).
"""
import midi
from orch import Piece, orchestra, held, sung, sweep, up


# --- the splashes' variations ----------------------------------------------------------------------------------

def victory4():
    """A minor's fourth over A, the Neapolitan B flat where A was due, E7 and A major at last.  The undertone: the
    second horn holding a low A under all of it (the major seventh under B flat, the fourth under E7), swelling, and
    the timpani rumbling under the call; the surprise: the call (D, E, F, A, from the downbeat, not Splash One's
    pickup) lifts a half step to B flat on B flat's chord, the bass and the timpani with it, then turns round A
    (G sharp, B) into A major's A, its third on the horns"""
    s = Piece(116, 2611, ['Dm/A', 'Bb E7', 'A'], level=0.0)
    ch = orchestra(s)
    br, hn, hn2, st, bs, tp, hp = (ch[k] for k in ('brass', 'horn', 'horn2', 'strings', 'bass', 'timpani', 'harp'))
    # the undertone: a low A held under all three bars, swelling slowly; the timpani rumbling under the call
    held(s, hn2, 0, 0, 11.6, 'A2', 80, e0=56, e1=124, e2=100)
    s.play(hn2, 0, 0, 3.9, 'E3', 70)
    s.roll(tp, 0, 0, 3.5, 'A1', 26, 58, step=0.125)
    # the call, from the downbeat; then B flat (the surprise) held and swelling, and the turn round A
    s.line(br, 0, 'D5:0:1.5 E5:1.5:.5 F5:2:1 A5:3:1', 92)
    held(s, br, 1, 0, 1.95, 'Bb5', 98, e0=110, e1=127, e2=100)
    s.line(br, 1, 'G#5:2:1 B5:3:1', 94)
    held(s, br, 2, 0, 3.6, 'A5', 98, e0=100, e1=127, e2=92)
    # the chords: D minor over A, B flat (the horns keep their D and F: B flat's), E7, A major
    s.play(hn, 0, 0, 3.9, ['D4', 'F4'], 82)
    s.play(hn, 1, 0, 1.9, ['D4', 'F4'], 88)
    s.play(hn, 1, 2, 1.9, ['D4', 'G#4'], 86)
    held(s, hn, 2, 0, 3.6, 'C#4', 90, e0=100, e1=127, e2=100)
    s.play(hn, 2, 0, 3.6, 'E4', 86)
    s.play(st, 0, 0, 3.9, ['A3', 'D4'], 70)
    s.play(st, 1, 0, 1.9, ['F4', 'Bb4'], 78)
    s.play(st, 1, 2, 1.9, ['E4', 'B4'], 76)
    s.play(st, 2, 0, 3.6, ['E4', 'A4'], 80)
    for b, beat, dur, q in ((0, 0, 3.9, 'A1'), (1, 0, 1.9, 'Bb1'), (1, 2, 1.9, 'E2'), (2, 0, 3.6, 'A1')):
        s.play(bs, b, beat, dur, q, 86)
    s.play(tp, 1, 0, .6, 'Bb1', 96)                # (the surprise struck)
    s.roll(tp, 1, 2, 2, 'E2', 40, 92, step=0.125)
    s.play(tp, 2, 0, .6, 'A1', 100)
    sweep(s, hp, 0, {2, 5, 9}, 57, 86, 54, beats=1.5)   # (up D minor into B flat)
    sweep(s, hp, 2, {9, 1, 4, 11}, 57, 88, 52, beats=3)  # (A major with its ninth, after the downbeat)
    s.crash(2, 0, 92)
    return s


def victory5():
    """B minor, then G minor's sixth over a bass falling B, B flat: the call climbs to B and sighs through B flat
    down to A on the D chord; the harp falls after it"""
    s = Piece(116, 2612, ['Bm7 Gm6/Bb', 'D'], level=0.0)
    ch = orchestra(s)
    s.line(ch['brass'], 0, 'D5:0:.5 F#5:.5:.5 A5:1:.5 B5:1.5:.5 Bb5:2:1.9', 96)
    held(s, ch['brass'], 1, 0, 3.6, 'A5', 96, e0=104, e1=127, e2=96)
    s.play(ch['horn'], 0, 0, 1.9, ['F#4', 'A4'], 84)
    s.play(ch['horn'], 0, 2, 1.9, ['E4', 'G4'], 86)
    s.play(ch['horn'], 1, 0, 3.6, ['D4', 'F#4'], 90)
    held(s, ch['horn2'], 0, 0, 3.9, 'D4', 80, e0=96, e1=124, e2=110)
    s.play(ch['horn2'], 1, 0, 3.6, 'A3', 84)
    s.play(ch['strings'], 0, 0, 1.9, ['B3', 'F#4'], 74)
    s.play(ch['strings'], 0, 2, 1.9, ['Bb3', 'E4'], 78)
    s.play(ch['strings'], 1, 0, 3.6, ['A3', 'D4', 'F#4'], 80)
    s.play(ch['bass'], 0, 0, 1.9, 'B2', 82)
    s.play(ch['bass'], 0, 2, 1.9, 'Bb2', 84)
    s.play(ch['bass'], 1, 0, 3.6, 'D2', 88)
    s.play(ch['timpani'], 0, 0, .5, 'B1', 76)
    s.roll(ch['timpani'], 0, 2, 2, 'D2', 36, 94, step=0.125)
    s.play(ch['timpani'], 1, 0, 1, 'D2', 100)
    s.run(ch['harp'], 1, 1.5, [86, 81, 78, 74, 69, 66, 62], 0.25, 64, rise=-2)
    s.crash(1, 0, 90)
    return s


def victory6():
    """C minor and F sus, the roll into G minor where B flat was due (the deceptive turn, the timpani striking G),
    E flat with its sharp eleven under the harp, and B flat at last, the call's A rising into it"""
    s = Piece(120, 2613, ['Cm7 F7sus4', 'Gm9 Ebmaj7#11', 'Bbadd9'], level=0.0)
    ch = orchestra(s)
    s.line(ch['brass'], 0, 'C5:0:1 Bb4:1:.5 C5:1.5:.5 Eb5:2:1 F5:3:1', 94)
    s.line(ch['brass'], 1, 'D5:0:1 G5:1:1 F5:2:1 A5:3:1', 96)
    held(s, ch['brass'], 2, 0, 3.6, 'Bb5', 96, e0=104, e1=127, e2=96)
    s.play(ch['horn'], 0, 0, 1.9, ['Eb4', 'G4'], 82)
    s.play(ch['horn'], 0, 2, 1.9, ['Eb4', 'Bb4'], 84)
    s.play(ch['horn'], 1, 0, 1.9, ['D4', 'Bb4'], 84)
    s.play(ch['horn'], 1, 2, 1.9, ['D4', 'G4'], 82)
    held(s, ch['horn'], 2, 0, 3.6, 'D4', 88, e0=100, e1=127, e2=104)
    s.play(ch['horn2'], 2, 0, 3.6, ['F4', 'Bb4'], 84)
    s.play(ch['strings'], 0, 0, 1.9, ['C4', 'G4'], 72)
    s.play(ch['strings'], 0, 2, 1.9, ['C4', 'F4'], 72)
    s.play(ch['strings'], 1, 0, 1.9, ['Bb3', 'F4'], 76)
    s.play(ch['strings'], 1, 2, 1.9, ['G3', 'D4'], 76)
    s.play(ch['strings'], 2, 0, 3.6, ['D4', 'F4', 'C5'], 80)
    for b, beat, q in ((0, 0, 'C3'), (0, 2, 'F2'), (1, 0, 'G2'), (1, 2, 'Eb2'), (2, 0, 'Bb1')):
        s.play(ch['bass'], b, beat, 3.6 if b == 2 else 1.9, q, 84)
    s.roll(ch['timpani'], 0, 2, 2, 'F1', 40, 90, step=0.125)
    s.play(ch['timpani'], 1, 0, .6, 'G1', 92)
    s.play(ch['timpani'], 2, 0, .6, 'Bb1', 100)
    sweep(s, ch['harp'], 1, {3, 7, 10, 2, 9}, 58, 91, 56, beats=2)
    s.crash(2, 0, 90)
    return s


# --- LOST2: the carrier lost again ----------------------------------------------------------------------------

def lost2_song():
    """the carrier lost, a second time round (Carrier Lost is tools/midi.py's lost_song): hollow fifths in B flat
    minor on the low strings, falling B flat, A flat, G flat, F and back, no third and no beat; the far horn takes
    up the way home's tune where Carrier Lost broke off (HOMELOSS's third and fourth bars: G flat, B flat, D flat,
    B flat, G flat, tools/flightsongs.py HL_TUNE) and stops short of its E flat; the oboe falls once from far above
    as the fifths sink to F, a timpani far off under it, once; the horn begins again and breaks off"""
    s = midi.Song(46)
    s.program(0, 48, 110)     # strings
    s.program(1, 60, 110)     # horn
    s.program(2, 68, 100)     # oboe
    s.program(3, 47, 84)      # timpani, far off
    hollow = [(0, 8, 'A#2', 'F3'), (8, 4, 'G#2', 'D#3'), (12, 4, 'F#2', 'C#3'), (16, 8, 'F2', 'C3'),
              (24, 8, 'A#2', 'F3')]
    for beat, dur, low, high in hollow:
        s.note(0, beat, dur - 0.1, midi.pitch(low), 84)
        s.note(0, beat, dur - 0.1, midi.pitch(high), 76)
    for beat, dur, name in ((2, 2, 'F#4'), (4, 1, 'A#4'), (5, 1, 'C#5'), (6, 3, 'A#4'), (9, 1, 'F#4')):   # the way home
        s.note(1, beat, dur - 0.1, midi.pitch(name), 94)
    for beat, dur, name in ((12, 3, 'G#5'), (15, 1, 'F#5'), (16, 4, 'F5')):                             # from far above
        s.note(2, beat, dur - 0.1, midi.pitch(name), 82)
    s.note(3, 17, 4, midi.pitch('F1'), 46)                     # far off, once, just after the fifths sink to F
    for beat, dur, name in ((24, 2, 'F#4'), (26, 1, 'A#4')):                                           # and it stops
        s.note(1, beat, dur - 0.1, midi.pitch(name), 80)
    s.note(0, 31.9, 0.1, midi.pitch('A#2'), 1)   # (the loop's length: the last bar is silence but for the strings)
    return s


# --- WIN2: "Stand Down", the won debrief -------------------------------------------------------------------------

W_HARMONY = ['Eb', 'Bb/D', 'Cm7', 'Ab Bbsus4', 'Eb/G', 'Abmaj7', 'Fm7', 'Bb7sus4 Bb', 'Cm7', 'Gm7/Bb', 'Abmaj7',
             'Abm6']
W_TUNE = ['Bb4:0:2 G4:2:1 Eb5:3:1', 'D5:0:3 Bb4:3:1', 'C5:0:1.5 D5:1.5:.5 Eb5:2:1 G5:3:1', 'F5:0:2 Eb5:2:2',
          'G5:0:2 F5:2:1 Eb5:3:1', 'Eb5:0:3 C5:3:1', 'Ab4:0:1 C5:1:1 F5:2:1 Eb5:3:1', 'Eb5:0:2 D5:2:2',
          'Eb5:0:1.5 F5:1.5:.5 G5:2:2', 'F5:0:1.5 Eb5:1.5:.5 D5:2:2', 'Eb5:0:2 C5:2:2', 'B4:0:3.6']
W_COUNTER = ['Bb3:0:4', 'C4:0:2 Eb4:2:2', 'F4:0:2 Eb4:2:2', 'Ab3:0:3.8']
W_BASS = ['Eb2:0:2 Bb1:2:2', 'D2:0:4', 'C2:0:2 G1:2:2', 'Ab1:0:2 Bb1:2:2', 'G1:0:4', 'Ab1:0:2 Eb2:2:2',
          'F1:0:2 C2:2:2', 'Bb1:0:4', 'C2:0:2 G1:2:2', 'Bb1:0:4', 'Ab1:0:2 Eb2:2:2', 'Ab1:0:3.8']


def win2():
    s = Piece(72, 2621, W_HARMONY, level=1.5)
    ch = orchestra(s)
    hn, hn2, tp, hp, st, bs = (ch[k] for k in ('horn', 'horn2', 'timpani', 'harp', 'strings', 'bass'))
    sing, oboe = 12, 13
    s.setup(sing, 48, 92)        # the strings that sing the tune's second phrase (the held ones go on under it)
    s.setup(oboe, 68, 96)        # oboe
    n = len(W_HARMONY)                              # 12 bars, 40 s
    s.arp(hp, range(n - 1), [0, 1, 2, 3, 2, 1], 0.5, 51, 75, 4, 50, accent=6)
    sweep(s, hp, n - 1, {8, 11, 3, 5}, 51, 87, 48)  # (A flat minor up into the top)
    s.pad(st, range(n), 55, 70, 3, 50)
    s.tune(bs, 0, W_BASS, 64)
    # the tune: the horn low, the strings up an octave over the horn's counter-line, the oboe over the horns
    sung(s, hn, 0, [up(t, -12) for t in W_TUNE[:4]], 82, e0=70, e1=122, e2=100)
    sung(s, sing, 4, W_TUNE[4:8], 76, e0=70, e1=120, e2=96)
    sung(s, hn, 4, W_COUNTER, 64, longest=2, e0=80, e1=112, e2=96)
    sung(s, oboe, 8, W_TUNE[8:], 80, e0=80, e1=124, e2=96)
    s.pad(hn2, range(8, n), 53, 65, 3, 58)
    s.play(tp, 0, 0, .8, 'Eb2', 52)
    s.roll(tp, 3, 2, 2, 'Bb1', 18, 66, step=0.125)
    s.play(tp, 4, 0, .8, 'Eb2', 62)
    s.roll(tp, 7, 2, 2, 'Bb1', 18, 58, step=0.125)
    s.play(tp, 8, 0, .8, 'C2', 56)
    s.end(n)
    return s


SONGS = {'VICTORY4.MID': victory4, 'VICTORY5.MID': victory5, 'VICTORY6.MID': victory6, 'LOST2.MID': lost2_song,
         'WIN2.MID': win2}
