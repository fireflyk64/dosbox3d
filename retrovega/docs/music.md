# The flight's music

What plays in space, how it is written, and how the game picks it. The
rooms' songs are in [engine.md](engine.md) ("Music"); the engine that plays
them all is `src/music.c` on the OPL3 (`src/opl.c`, our bank
`tools/oplbank.py`).

## The user's brief (2026-10-23)

- **Space:** many songs, pensive but curious, like the Vega Strike pieces
  they played us (terranpeace, terranpeace2, peace4, peace5_p).
- **Home:** a very calm one for the way home, once the mission is done and
  the carrier is targeted.
- **Battle:** like terranbattle.
- **Stings:** three short victory songs for an enemy splashed, and a short
  tragic one anyone would know again.
- **Panic:** three songs for a ship falling apart, from tense to full panic.
- **Length:** not long songs, but some 15 to 30 minutes of music in all.
- **Reference:** the songs are OPL MIDI, so the references were inspiration
  only.

**What the references have in common** (librosa, chords every two beats):

- **Harmony:** minor and modal (B minor, C minor), sus chords and open
  fifths, pedals and drones (peace5_p is C minor throughout), i-VI-VII and
  i-v-VI.
- **Texture:** arpeggios turning under a sparse tune, and layers coming in
  one by one.
- **Tempo:** 95-110, or half time.
- **Battle:** terranbattle is a C sus pedal with hits on every beat, Fm,
  Gm and B flat passing over it.

The songs are our own, in that language. They are not copies.

## The user's notes on the first draft (2026-10-24)

They listened through a preview page and wrote notes at times. In short:

- **Cruise:** too casual in places ("Pokemon", "bar music", "cheap"), and
  they wanted an "orchestral feeling of wonder". Builds had no payoff.
  Intros were too quiet for loops.
- **The way home:** one song, looping, starting at once, never sleepy,
  "dopamine and relief".
- **Battle:** Contact perfect, Furball nearly. All the combat songs should
  be "lego pieces" that loop perfectly or combine. Break and Attack was
  repetitive, "racing music". Crossfire was lethargic, but would suit
  asteroids.
- **Panic:** Hull Breach must keep the fight's pace, with the damage as
  dissonance over a fight. Systems Failing's builds need payoffs. Abandon
  Ship should run panic, hope, dashed, hope, dashed; its speed-up was
  "brilliant".
- **Stings:** too happy, they want longing. The loss was not tragic
  enough: it should be a recognisable dirge.
- **Their question:** intro tracks and loop tracks? One file each, with a
  loop marker the engine follows (below), does it at the tick.

## The third draft (2026-10-24)

The user's verdict on the second: the cruise songs "sound like elevator
music--not memorable". They sent two cues from another score for the
feeling of excitement and wonder, and ruled that ours stay our own: the
cues set the feel, nothing is taken from them note for note. Their program
numbers are MT-32 tones, not General MIDI (the user spotted it): synth
brass, French horns, strings, timpani, piccolo, harp and an orchestra hit.

Measured against ours, the cues were fast (165 in 4/4, 123 a dotted beat
in 12/8, against our 72 to 108), lean (three parts sounding against six
to eight), restless (a note in every fifth of a second), chromatic (15%
of their note time outside one key: lifts of a major third, minor-third
cycles), sparkling (one to three fast notes a second: runs, sweeps), hit
where we had a beat or nothing, and short (30 to 40 s loops ending in a
riser). The plan: a new set of cruise pieces in that style and the way
home in two versions, step 1 first for the user's verdict.

**Step 1** (commit 744050f0) built the orchestra, Outbound and the way
home. The user: Outbound "hits on all counts... fix all the flight music
to have that same style but their own unique character and be remixable
and loopable so the orchestral score is complete". **Step 2** did that:
every flight piece rewritten for the orchestra, and the score's remixes.

- **The orchestra** (`tools/orch.py`) on programs no room song uses:
  spiccato strings (44) for the motor, an orchestra hit (55), a piccolo
  (72), a synth brass that bites (62, 63); with the horns, strings,
  timpani and harp. The crash is 6 dB down, the user's note: it stood 4 dB
  over the mix (A-weighted).
- **The balance** of the parts is the reference render's (the user liked
  it), measured K-weighted against the motor: the brass 8 dB over it, the
  horns 5, the piccolo 4, the hits 7, the crash 5. `ORCH` sets each
  part's channel, program and level; `orchestra(s)` puts them on.
- **The guardrail:** `tools/songcheck.py` measures every flight song
  against bands from the two cues (notes a second, motor, voices, off-key
  share, fast notes, drum hits, bars without a change), the fights and the
  rock field in bands of their own, and checks the markers: a loop marker,
  blocks on bar lines, at least four blocks in a fight, nothing just past
  a bar line. Every piece passes.
- **The user's notes on the first full set** (2026-10-24, timestamps on
  the listening page): Sensor Sweep's scan of note pairs with the piccolo
  asking and the muted trumpet answering was "too childish like random
  buttons", "too kings questy"; Deep Lane's D flat block (second pass)
  "loses the thread... should tighten up a lot and be moodier"; Waypoint's
  horn answers "too repetitive and happy", its stabs and its way home "too
  happy and jumpy", "too much like a video game", while its lift "gets good
  again"; Starwake "feels somehow repetitive with the rest... we need
  something special here". Each of those passages is rewritten; what they
  liked (Sensor Sweep's G major, Waypoint's lift) is kept. Starwake went
  into 3/4 (`Piece(..., beats=3)`: the figures take the bar's length from
  the piece, which stays).
- **Then** (the same day): Systems Failing and Abandon Ship "a bit too
  repetitive with themselves... somehow you get stuck in this loop": "cut
  in pieces of the battle various musics and just add little spurts of
  panic to an otherwise recognizable song", baked into the songs ("we
  don't necessarily need to invent a new music player technique"). So
  each quotes the three battles' blocks note for note (`orch.excerpt`,
  with the channels' programs and controllers chased; `orch.restore`
  after) with two-bar bursts between them. And Starwake "too happy...
  maybe we should simply delete it... unless we can really soup it up
  with some very pensive minor pieces and some very fast minor pieces
  with chimes": rewritten in D minor, laments and runs in turn, with a
  new chimes patch (tubular bells, program 14).

## The songs (CC0, ours)

| cue | title | key, tempo | what |
|---|---|---|---|
| CRUISE1 | Outbound | E minor, G# minor, C, E; 160 | launch: the motor in octaves, a synth-brass call and horn answers, hits on 3+3+2, a break into C, the horn's tune, home to E major, a timpani riser |
| CRUISE2 | Wide Orbit | G minor, 12/8 at 116 | wonder: harp rolling two octaves over a turning spiccato, the horn singing, round the minor-third cycle G, B flat, D flat, E minor |
| CRUISE3 | Sensor Sweep | B minor, 152 | searching: the spiccato's beam (the bass, and a line up the chord and back every two bars) under the horn's rising fourths, then G major opens for the horn's tune with the hits |
| CRUISE4 | Far Light | D, 12/8 at 124 | heroic wonder, the brightest: strings galloping under harp sixteenths, the synth-brass fanfare lifted D, F#, B flat |
| CRUISE5 | Deep Lane | F minor, 144 | dark wonder: low strings in 3+3+2, the horn's tune falling through D flat and G flat, a darkest block (B flat minor to C7) of one falling figure on the 3+3+2 with the timpani, diminished piccolo flourishes |
| CRUISE6 | Long Burn | A minor, 12/8 at 112 | anticipation: eight bars of repeated A's burning under turning chords, then the release into C# minor, the strings' tune |
| CRUISE7 | Waypoint | C minor, 168 | brisk travel: the strings at a gallop over a pedal C, the brass's call in long notes over the horns' falling line, a lift through A flat and B flat, home through A flat and B flat to C |
| CRUISE8 | Starwake | D minor, 150 | pensive and very fast in turn: chimes tolling over the horn's lament, then runs (sixteenths, the chimes' fast figure, the horns falling D, C, B flat, A); the lament darker in G minor on the strings, the runs wilder round the Neapolitan E flat; chimes tolling A into the top |
| CRUISE9 | Apogee | F# minor, 12/8 at 120 | wonder, Wide Orbit's sibling: the harp climbs the chord for a bar and falls the next over a spiccato turning like a wheel, the horn singing a fifth up and holding, round the major-third cycle F# minor, D, B flat, back through B minor and the Neapolitan G |
| CRUISE10 | Faint Signal | A minor, 152 | searching, Sensor Sweep's sibling: the spiccato stacks root, fifth and ninth in 3+3+2 while the roots fall by thirds, the horn reaching up in fifths; the harp sweeps up E's chord and the signal is found in C, lifted to E flat; now and then a run falls instead (the first piccolo strum, the second pass's sweep into the signal), once rising and falling (an A), once falling and rising (a V) |
| CRUISE11 | Nightside | C# minor, 146 | dark wonder, Deep Lane's sibling, its first take now a faint bed 8 dB down (the spiccato falling in 3+3+2 ripples, one phrase sinking a major third twice: C# minor, A minor, F minor with the timpani, C7 over E into C# minor; whole-tone piccolo flourishes) under a trumpet tune of its own: long notes and rising quarter-note triplets, its peak a half step higher each block up to the payoff's high C#, the brass an octave under it the second time |
| CRUISE12 | Starfall | E minor, 150 | Starwake inverted, with far less chiming: its lament upside down, its falling drive climbing, its chime figure falling threes on the harp, its rising piccolo runs falling in 32nds; the second lament a major third down in C minor |
| LAUNCH2 | Catapult | C minor into E, 156 | the deck's pulses on C with the timpani's heartbeat and the horn's call; the catapult held on B, the B's doubling, a run up three octaves, horns swelling on B7; then E major, Outbound's octave motor, the brass fanfare, a lift into A flat |
| HOMEWARD | Homeward, won | B flat, 12/8 at 108 | harp eighths, the horn's tune, a lift into D for a brass chorale, home through B flat |
| HOMELOSS | Homeward, lost | B flat minor, 12/8 at 92 | the same, slower and sadder: horns for the brass, no hits, sinking a major third to G flat |
| BATTLE1 | Contact | C sus pedal into C minor, 140 | the call on the horn answered by synth brass over spiccato sixteenths, low strings and timpani |
| BATTLE2 | Break and Attack | C minor, 140 | half-time hits over the motor at full speed, a riff of its own on strings, a Neapolitan horn tune, a lift into A flat |
| BATTLE3 | Furball | C phrygian, 140 | the riff on synth brass and horn over driving fifths, up a minor third the second time, a tutti stop |
| BATTLE4 | Intercept | C minor over a C pedal, 140 | Contact's sibling: the synth brass calls (a short note leaping to a long one) and two horns answer in thirds over galloping low strings and the spiccato; the strings' long tune to a high C, a dark horn chorale, the call and answer for the whole orchestra |
| BATTLE5 | Check Six | C minor, 140 | Break and Attack's sibling, with apprehension on the downbeats: a bass falling by half steps (C, B, B flat, A flat, G) lands on every downbeat on the low horn, low strings and timpani; the brass's calls sigh down a half step; the horn's F minor tune over the riff's pulsing strings and hits; a darker block in E flat minor; the last block a long-note tune of sighs on the strings and horn over the riff |
| BATTLE6 | Scissors | C phrygian, 140 | Furball's sibling: two lines from one C over driving fifths, the brass climbing and the horn falling, open to the octave on 3+3+2 and snipped shut; the horn's tune over F minor, E flat, D flat, C; the blades drawn out in long notes over A flat, B flat, G7, C minor, a climb a step a bar into a stop on F minor; a stop on 3+3+2 with toms |
| ROCKS | Rock Field | G minor, 120 | a march through the rocks (snare, timpani, low strings), the horn's theme, a drift through B flat with the harp and piccolo glinting, a brass descant |
| ROCKS2 | Drift Field | D dorian, 118 | exploring, not marching: the spiccato drifting in sixteenths, the horn's tune wandering up; rocks going by, string whooshes, harp glissandi and timpani rumbles panned across, a soft tom where each passes closest, a few near misses with a crash; the middle through B flat and G flat lydian |
| PANIC1 | Hull Breach | C, 140 | the fight going on under the damage: a low cluster, alarms in seconds, the call a tritone off |
| PANIC2 | Systems Failing | C, 140 | the fight breaking up: the battles' blocks quoted in turn (Contact's call and answer, Break and Attack's riff and Neapolitan, Furball's riff and surprise), two-bar bursts between them: the half-step climb, wild tritone bars, the power failing, the alarm |
| PANIC3 | Abandon Ship | 140 to 200 | shorter quotes, wilder bursts: panic in tritones, hope dashed, dashed harder, a cluster, the call a tritone off; the last six bars speeding up into the loop |
| PANIC1B | Damage Report | C, 140 | Hull Breach's calmer sibling: the motor on its C pedal, low strings on 3+3+2, a timpani heartbeat, a sighing horn tune over the pedal, Contact's call at half speed, one small burst of alarm in eight bars |
| PANIC2B | Reroute Power | C (F minor in the middle), 140 | Systems Failing's focused sibling (the user: "so the player has time to make it right"): one steady pulse under a four-bar chord cycle, one clear step-step-hold tune on the horn, the brass, the strings, then everyone; the alarms short, once a phrase |
| VICTORY1..3 | Splash One..Three | C, D, B flat | bittersweet: bVI-bVII-I, the minor plagal, a deceptive turn |
| VICTORY4 | Splash Four | A, 116 | its own, 6 s (the user: "kinda repetitive for the previous--maybe 2 seconds longer with a surprise or undertone"): D minor over a low A held on the horn, the timpani under the call, which lifts to B flat where A was due; E7, then A major with the crash and the harp |
| VICTORY5..6 | Splash Five..Six | D, B flat | Splash Two's and Three's siblings: the minor plagal over a bass falling B, B flat; the deceptive turn struck on G with the timpani |
| TRAGEDY | Wingman Lost | B flat minor, 52 | a funeral march, 14 s |
| LOST2 | Empty Deck | B flat minor, 46 | Carrier Lost's sibling (tools/stings2.py): hollow fifths falling B flat, A flat, G flat, F; the far horn picks up the way home's tune where Carrier Lost broke off and stops short; the oboe falls once from far above |

Some 34 minutes of music, every piece its own, each going round from
its loop. `tools/flightsongs.py` has Outbound and the way home and gathers
the rest from `cruise_a.py`, `cruise_b.py`, `battles.py` and `panics.py`,
and their siblings from `cruise_c.py`, `battles2.py` and `panics2.py`
(the user's soundtrack review, 2026-11-17: "every combat and flight and
victory and lost needs another variation"), all written with
`tools/orch.py`; `tools/stings2.py` has the splashes' siblings, Empty
Deck and the won debrief's Stand Down. A song carries at most 12 "sect"
markers (`midi.py` refuses more, `music.c` MAX_SECT), and its name is a
DOS name, eight letters at most (`rvdata.py` asserts it: QUARTERS2 would
have opened QUARTERS.MID). `orch.in_time(piece)` (2026-11-17's third pass) lets a note
go only for a note of its pitch that starts while it sounds: `songs.Comp.note` lets go the one written last,
even one that starts later, so a block written in the code before an earlier one lost notes of the same pitches
to a tick (Check Six's last block, the riff tripled an octave up over orchestra-hit blips, was the user's
"gameboy boss music" in part). The pieces revised since use it, but for Drift Field (its notes back put it over the
voices band); the others still have their cut notes (Intercept 80, Furball 48, Drift Field 35, Starfall 28, a
few in others) until the user hears them fixed.

- **The cruise cycle:** each cruise piece's riser lands on its own
  dominant, and the next in the cycle starts in its key: Outbound in E
  minor, then G minor, B minor, D, F minor, A minor, C minor, D minor (Starwake),
  F# minor, A minor, C# minor, E minor (Starfall), and round to Outbound.
- **Blocks:** `Piece.sect(bar)` (or `sections`) writes a "sect" marker at
  each block. A fight's block stands alone, so it can follow any block of
  any fight: it starts on its downbeat with the motor and a hit, in the C
  minor family, and no note is held over its end.
- **Loops:** a "loop" marker meta event (`FF 06 "loop"`) after a song's
  intro is where it goes round again from. The engine plays the intro once
  (`music.c`: the marker becomes a do-nothing event, `Song.loop` its index;
  the ring wraps there and that event's delta is zeroed, since the last
  bar's rest has brought it to the bar line). `oplrender` follows it too.
- **Levels:** evened out by `level=`: the way home at -25.6 to -24.6 LUFS,
  the cruise at -24.5 to -23.4, the rock field -22.8, the fights -22.2 to
  -21.8, the stings -20.8 to -19.1.
- **Voices:** none steals a voice: at most 18 sound at once.
- **Whole bars:** a loop's length is its last event's bar, so nothing may
  sound past the last bar line: ramps, swells and sweeps end their last
  step inside it.

**Previews:** `build/oplrender --bank build/dos/RVOPL.BNK --mid
build/dos/CRUISE1.MID --out CRUISE1.wav` (tools/oplrender.sh builds it)
plays a song through the game's own driver. `RV.EXE /midilog` records what
the game sent, and `oplrender --log OPLLOG.BIN` plays that back.

## Picking them: src/score.c

The score only watches. Once a frame (`flight_music`) it reads the ships'
slots and the ship's systems (panel.h), so combat need not tell it
anything. The rules come in this order:

1. **Dead** (the player's ship dying, `shipAiState` 9): the tragedy,
   once, and then quiet. The hull gone with the reserve left (damage.md)
   is not dead but the third panic; it used to be "hull 0", and the
   tragedy played on the reserve with the player still flying.
2. **Panic:** with the hull under 60%, 35% and 15%, the first, second and
   third panic song; the first two each have a sibling (Damage Report,
   Reroute Power), one or the other by the music's dice. A panic stays,
   since the hull does not mend in flight.
3. **Fight:** an enemy within 4,000 km starts a fight, a little beyond the
   3,000 km of its hail. Twelve seconds with none within 6,000 km end it.
   They spawn some 5,000 km out, so a flight begins calm.
4. **Home:** the mission is done, or the player is calling the carrier
   to land (C). Done is the logic's `victoryPoints` (the fixed block's
   word the layer carries to the other players at the mission's end, as
   in WC1): 0 from the launch, nonzero once every nav point is reached and
   every enemy of the mission killed (logic.c `step_mission`). Done,
   Homeward; called home with it undone, Homeward's sadder twin. At the
   end `missionVictoryCalc` is the layer's moment to put the host's score
   in, and the debrief follows it. It used to be "every enemy in the slots
   gone, after there were some", and a friendly capital ship targeted: a
   nav point cleared is the first, so Enigma 1 played the sad twin, a
   slow lament in B flat minor, from Nav 1 to Nav 2 (the user,
   2026-11-03: "the funeral sound plays when I go from nav 1 to nav 2 in
   mission 1"). `tests/navmusic.txt` flies Enigma 1 nav point to nav
   point (`$` in RVT.EXE kills the enemies about): cruise, the rock
   field, a fight, cruise, and Homeward only after Nav 3. The way home's
   song plays once, then two of the cruise songs in their turn, then it
   again (`HOME_EVERY` 3, `home_next`; the user, 2026-11-17, of Homeward:
   "This piece gets old very fast ... only cycling through this ... every
   3rd song").
5. **Rocks:** inside an asteroid or mine field of the mission (its centre
   within its radius), Rock Field or Drift Field by the dice.
6. **Calm:** otherwise, the cruise songs.

On top of these:

- **Stings:** an enemy within 12,000 km that begins to die (`shipAiState`
  9) was splashed, and gets a victory sting, the six in turn, the turn
  begun by the dice when the game's first flight starts (from
  2026-11-17: it began at Splash One at every start of the game, and a
  session of a kill or two heard only that one, the user's "always plays
  the *same* default song"). A friend
  dying the same way gets the tragedy, and a capital ship of ours the
  carrier's loss, LOST or LOST2 once (the cues CAPLOST, CAPLOST2; the
  user, 2026-11-17: "We should play carrier lost whenever any capital
  vessel is lost"). A ship gone from the slots without
  dying (left behind by the autopilot and held out of the scene, landed,
  jumped out, run off) is no loss: it had been, and the tragedy played as
  the autopilot reached nav 1 (the user, 2026-11-02).
- **Remixes** (`music_then`, the engine's follow-ons): as each calm song
  comes in, the next of the cruise cycle is asked for after its pass,
  from its loop, so the riser of one lands on the downbeat of the next,
  spliced on the bar line with no fade. As each battle comes in, another
  is asked for at one of its own blocks, a block or two past what the
  ring holds: so 20-35 s (three to five blocks) of each before the next
  cuts in. The place in each list is kept from flight to flight, and a
  run's first flight starts each list where the campaign has got to: the
  cruise list five songs on for every mission flown (`CRUISE_STEP`: five
  and twelve share no factor, so each song in turn), the battles (six) one
  on, both offset by
  the campaign's seed (score_start; the user, 2026-11-08: "it seems the
  game favors some songs over others. I feel we need that variety": every
  run had begun at Waypoint and Battle 1). A panic
  comes in on the bar. A sting fills the ring with itself, and the
  follow-on asked for goes with it: the same one is asked for again once
  the song is back. The list moves on only for a follow-on the engine
  took (`music_then` says), and one asked for but never heard (the mood
  changed first) stays the next of its list. It had moved on for every
  frame a sting's ring refused one, so after Nav 1's victory sting
  Outbound went on into Far Light instead of Wide Orbit, its riser on the
  wrong dominant (2026-11-03).
- **The splice** (`music.c`): music_poll keeps a ring some 20 s ahead of
  the interrupt. When its fill reaches a follow-on's point (the end of the
  pass, or a block's marker, the bar line reached), it puts every
  channel's notes off, a switch event, the new song's channels as they
  stand at its first event (the chase), then its events. The interrupt
  takes the switch on the bar line: the new song, its bar clock and its
  event count from there. `music_then_cue` says a follow-on is still to
  be heard until then.
- **Whole bars** (`load_song`): a rest after a song's last event runs to
  the end of its last bar, found in the file's ticks and timed by its
  tempo. It used to be counted on the music clock, a beat rounded to whole
  ticks: where the beat rounds down (116, 124, 108, 156: 156 is 92.3 ticks
  a beat, counted 92) the bars came short and the rest grew nearly a bar
  too long, some 2 s of silence at the loop and before a follow-on; where
  it rounds up, a hitch of 0.2 s. The bar grid that cues wait for is kept
  in 1/256 ticks now, so a cue on the bar lands on it however long the
  song has played.
- **Cross-fades:** calm, home and the rocks fade over 1.5-2.5 s on the
  bar; a fight starts over 0.7 s on the beat.

**A sting** (`music_sting`, cues from `RV_MUSIC_STINGS` on, played once):

1. The engine notes where the song it cuts short was: the interrupt keeps
   the song's event index as it takes events, chase events aside.
2. It plays the sting.
3. When the sting ends, it goes on with the song from there. First it plays
   the channels' programs, volumes, pans and expressions as they stood
   there (the chase: the song read from its start to that event).
4. The song fades back in over 300 ms.

A sting over a sting goes back to the first one's song. A cue that comes
during a sting waits for it to end.

**Test:** `RVT.EXE /scoretest` (the test build) drives the moods by the clock instead of
the flight. The schedule, in seconds:

| time | what |
|---|---|
| 0 | calm |
| 8 | a rock field |
| 14 | a fight |
| 22 | the next battle chained on |
| 30 | a victory sting |
| 38 | the tragedy |
| 52, 60, 68 | the three panics |
| 78 | home, won |
| 88 | home, lost |
| 98 | dead |

`tests/score.txt` flies it (`tests/run.sh tests/score.txt /scoretest
/flight 105 /midilog`). `/scoretest 2` is the remixes': calm for 96 s
(Outbound's pass, then the next cruise piece spliced on), a fight for
100 s (the battles cut together block by block), the way home
(`tests/remix.txt`, `/flight 220`). `/scoretest 3` is the cycle's end:
calm only, from Nightside, Starfall spliced on after it, then
Outbound (`tests/cycle.txt`, `/flight 160`). The music log's lines carry the music clock
(`@ticks`, 240 a second), so a log rendered with `oplrender --log` can be
cut and labelled.

**The room songs' starting places** (2026-11-17, the user, of the lounge: "people never stay in this room for long and
so we want them to hear different parts of the music"; of the bar: "4 different sensible start points in this song and
the original"): Bunk Light, Off-Watch and Off Duty carry a "sect" marker at each later section's start (tools/songs.py:
four, four and five; Dog Watch four, Quiet Hours three). Entering a room starts its song at its own start or at one
of those, by the dice (`game.c` `room_music`, `music_cue_sect`: the instruments chased to that event); the same song
already playing goes on. The deck's starts at its top: Catapult's blocks are the deck, the catapult, the thrust in turn.

**The dice** (`music.c` `music_pick`): a generator of the music's own, stirred by the music clock on every pick. The
clock goes in before the multiply: added after it, a session's first picks (the clock a few thousand ticks, the state
still 0) left the bits read at 0, and every first room, launch and splash took the first of its kind. The music only
watches the game, so no machine's world depends on what it picks. Families (`game.c` `family_cue`): QUARTERS or
QUARTER2, BAR or BAR2, LAUNCH or LAUNCH2, WIN or WIN2, LOST or LOST2, one of a family already playing kept.

**Memory:**

- **Loading:** the 49 songs load at start-up into XMS, some 146,000
  events (860 KB; 95,000 and 570 KB for the first 28). Each has a
  descriptor in conventional memory (36 bytes, its 12 blocks' places in
  it; 24 until the siblings, when 12 was the most any song had) and a
  name (13 bytes): 2.4 KB for the 49. RV.EXE keeps the descriptors
  fixed; the test builds take them from the heap in `music_init`, as
  RVP.EXE's play tests start no music and its heap had no 1.7 KB to spare
  (the shot list found no room). The far heap grows to read each song's tracks, and
  `_fheapshrink` gives that back to DOS afterwards: 283 KB is free after
  the music (the walk test, 2026-10-24), where it was 180 KB with the seven
  room songs.
- **`/midilog`:** its buffers used to be taken from the far heap before
  anything else, and then the fonts found no room. That was already so
  before the flight's music. Upper memory is the ships' two dense
  buffers', to the byte. So the log now begins at the first `music_poll`,
  with 1,024 entries written out every frame. It writes the chip's set-up
  into the log first, and `opl_uncache` has every voice's patch written
  again at its next note, so `oplrender --log` plays it back right.

**Still to do:** combat (hits, kills, the hull's damage), the carrier in
space to target, mission objectives. The score already listens for all
of them.
