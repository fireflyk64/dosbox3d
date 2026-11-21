# Enigma 4, the tunnels (ENIGMAT): out of the campaign

Taken out on 2026-11-20 at the user's word, after flying it: "the enemies seem stuck on the roof the whole time. they
don't really fight and they are annoying to hit. can we disable the entire mission for now and simply continue to the
next set of missions as if it weren't there".

## What was changed

One line, `data/campaign.csv`, the ENIGMA row's mission list:

    before: ENIGMA,1,0,ENIGMA1 ENIGMA2 ENIGMAP ENIGMAT,CEPHID,SHELTON,,10
    after:  ENIGMA,1,0,ENIGMA1 ENIGMA2 ENIGMAP,CEPHID,SHELTON,,all

A mission is flown, and listed in the page's mission menu, only when its set's row names it. Nothing else was removed:
the mission's text is still in `data/missions.txt` (`mission  ENIGMAT`), the tunnel's ground in `src/terrain.c`
(`TR_STAMP_TUNNEL`, `tr_tunnel_at`), the AI's tunnel flying in `src/logic.c` (`tunnel_mind`), the test pilot's in
`host/bot.c`, and its play test M8.71 still runs (it launches the mission by name).

## To put it back

1. Add ` ENIGMAT` after `ENIGMAP` in that row.
2. Nothing else: the row's `all` (the set's pass mark: every point its missions give) counts the tunnels' two points
   in by itself (tools/rvdata.py pass_mark).
3. Build (`./build.sh`), run `tests/play/playtest.sh`, deploy.

Saves: a save holds its set and its place in the set's list. ENIGMAT was the last place, so no other place moved. A
save standing on it now falls back to the envoy mission (campaign.c `camp_settle`), flown once more.

## What to fix first

- The Agis and Nicanders inside sit under the vault and hardly fight (the user's report). `tunnel_mind` only keeps
  them out of the rock; `step_ground` undoes a step into rock and never brings them back to the middle; the usual
  tactics want room the tunnel does not have. They need a tunnel tactic of their own: hold the middle line, come at
  the player along it, fire when lined up.
- The test pilot never won it (0 of 4, kills 0 to 2: host/bot.c).
- Without the sprite card DOS draws the inside at some 17 frames a second.

## The points, the same day

The user's rule replaced the old game's points (docs/gameplay.md, "The points"): a mission gives one point for its nav
points flown and the landing, one for its capital ships protected, one for its capital ships destroyed, each all or
nothing; a set's good way needs every point ("No slips for now"). Enigma: 1 + 2 + 2 = 5; with the tunnels 7.
