# The checklist: the old game's firing exceptions, targeting and locks, the VDUs, the nav and comm menus (2026-10-31)

The user's brief (2026-10-31), after the held-fire mechanism was re-read in the old game's code (tests/wc1/README.md):
the ace's exemption from the held fire stays as it is (`PF_NO_MERCY` on `P_ACE`); the other exceptions go back to
the old game's shape "since it seems to be fun"; the target key selects only what is in front of the pilot and
there is a lock; the wingman takes orders through a comm menu; the nav key opens a nav menu and the chosen nav
point shows as a white cross; T (even with the target unchanged) puts the target's top-down wireframe with its
shields and hull on a VDU; D a damage readout, and the damage and weapon-select displays show the player's own ship
the same way with the live guns and the chosen missile; a wrecked target-tracking system means no missile lock and
no target lock; a Top Gun style missile lock display (a heat seeker locks from behind only, friend-or-foe and
dumbfire "just point generally at the target": one seeks the nearest, the other flies straight).

The user asked for this plan to be in the repository and for a commit after every item (checkpoints: their
connection to the service is unreliable). Each item below is one commit; the state column is kept current.

| # | item | state |
|---|---|---|
| A1 | the held fire withholds missiles and mines too (the old wrapper skipped the whole weapons routine): `holds_fire` gates the lock and the launch in `aim_and_fire` | done (M5.17) |
| A2 | the missile dodge fires without the hold: a missile homing on the ship from within 37 degrees ahead and under 8,000 km, the ship fires its guns at its target whatever the hold (the old maneuver 32) | done (M5.18) |
| A3 | the approach is silent: a new tactic `T_APPROACH` (the old 40: to the point 500 km plus the radius behind the target, above it when it faces the ship; no guns; the tail, `T_PURSUE`, once behind within the radius plus 2,000 km with the target facing away) and `T_CLOSE` (the old 45: `T_PURSUE` unless the target faces the ship within 37 degrees, then `T_APPROACH`); the fighter's rows pick them as the old table did; M5.13/M5.14 re-staged from behind | done (M5.19; M5.13/M5.14 re-staged) |
| B1 | the target key: only ships in front of the pilot (ahead of the wing line), the nearest to the nose first, each press the next round; in logic.c (`logic_target_next`) so the play tests cover it; a target that dies is dropped | done (M10.1) |
| B2 | the target lock: L locks the chosen target (kept through T and AUTO until unlocked, lost when it dies); no lock with the target-tracking system wrecked | done (M10.2) |
| B3 | the player's systems as the old game's nine: power plant, shield generator, computer, intercom, target tracking, acceleration absorbers, ejector, repair, and the guns; a hit through the armour wrecks one a point (`do_damage`); the track wrecked: no target lock, no missile lock | done (M10.3) |
| C1 | the target VDU: the target's top-down wireframe (its silhouette's outline, collide.c's top view) with its front and rear shields and four armour faces as bars and its hull, on the right VDU; T shows it even when the target did not change | done (src/vdu.c; a flight screenshot checked) |
| C2 | the damage VDU (D): the player's own wireframe with the damaged systems listed, the wrecked ones red | done (vdu.c draw_damage; screenshot checked) |
| C3 | the weapon VDU: G cycles the guns (all, each in turn), W the missiles; the own wireframe with the live guns lit at their mounts and the chosen missile named; the fire loop fires the chosen guns and launcher | done (M10.4; vdu.c draw_weapons; screenshot checked) |
| D1 | the nav menu: N opens the nav page on the left VDU (the nav points listed, the chosen one lit), N again the next; the chosen nav point drawn as a white cross in the view where it lies | done (M10.5; vdu.c draw_nav; screenshot checked) |
| E1 | the comm menu: C opens SEND MESSAGE TO? on the left VDU (the wingman, the carrier), then the orders (attack my target, break and attack, form on my wing, help me out, return to base, never mind); digits pick, Esc closes; request landing under the carrier | done (vdu.c draw_comm, flight.c comm_key; screenshots checked) |
| E2 | the wingman's orders in its mind: formation (`T_FORMATION` until ordered off), attack my target (the player's target, pursued wherever it goes), break and attack (free engagement, the default), help me out (whoever tails the player), return to base (the leave to the carrier); it answers on the comm with its own voice | done (M10.6; logic_order) |
| F1 | the missile lock: the chosen missile's seeker (weapons.csv `lock`): heat only from the target's rear arc, image with the nose on it, friend-or-foe at once on the target else the nearest enemy, dumbfire never (straight); the lock builds over `lock_s` with the target in the seeker's cone and is lost out of it; none with the track wrecked | done (M10.7; logic.c lock_tick) |
| F2 | the lock display on the HUD: a Top Gun style reticle closing on the target as the lock builds, solid when locked, with the seeker's name; the lock lamp follows it | done (logic_hud: the closing diamond, the double one locked) |
| G1 | the page's key list (web/gamefiles.js `keys`) and the phone buttons (web/touch.js) for the new keys | done (gamefiles.js keys; touch.js Target, Comm, Missile buttons) |
| H1 | docs: docs/ai.md (the new tactics and the orders), docs/engine.md (the VDUs, the locks), docs/gameplay.md (the tests) | done |

Each item that touches the logic gets a play test (tests/play/playtest.c) run on the host and in DOS
(`tests/play/playtest.sh --dos`), and the deploy to the page follows each commit that changes RV.EXE.

## Afterwards: the comm menu rebuilt (2026-10-31, the user's ask)

The menu of E1 and E2 gave way to the user's layout: 1 the wingman at all times, 2 the target; the wingman flies in
formation or breaks formation; an enemy target can be taunted; the home carrier as the target takes the request for
landing, and grants it with the mission finished or a nav point flown. So every mission now has the Vigilance in it
(`ship VIGILNC 0 nav0 role=home`), and the comm faces carry subtitles. docs/engine.md "The VDU pages", docs/ai.md,
M8.13, M8.14, M10.8, M10.9.
