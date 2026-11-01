#!/usr/bin/env python3
"""The script as a page to read and comment on, from data/missions.txt and data/campaign.csv: every line of every mission,
who says it and when it plays.  tools/scriptpage.py OUT.html"""
import csv
import html
import os
import re
import shlex
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
NAME = {'FROST': 'Frost', 'KESTREL': 'Kestrel', 'DRIFTER': 'Drifter', 'PRIME': 'Prime', 'ANCHOR': 'Anchor', 'TORCH': 'Torch',
        'COMET': 'Comet', 'ECHO': 'Echo', 'TALLY': 'Tally', 'TECH': 'Kit', 'XO': 'Solberg', 'you': 'Wildcard'}
SYSTEM = {'ENIGMA': 'Enigma shipyards', 'CEPHID': 'Cephid 17', 'SHELTON': 'Shelton', 'CRUCIBL': 'Crucible picket',
          'OLDZIEY': 'Oldziey', 'DEFIANC': 'Defiance', 'LIXIS': 'Lixis beacon', 'ATARAXI': 'Ataraxia', 'RIGEL': 'Rigel relay',
          'BARNARD': "Barnard's Star", 'WOLF359': 'Wolf 359', 'SHMRN': 'Shmrn pocket', 'LUYTEN': "Luyten's Star",
          'GOOD': 'The war is won', 'BAD': 'The war is lost'}
OLD = {'ENIGMA': 'Enyo', 'CEPHID': 'McAuliffe', 'SHELTON': 'Gateway', 'CRUCIBL': 'Gimle', 'OLDZIEY': 'Brimstone', 'DEFIANC': 'Chengdu',
       'LIXIS': 'Dakota', 'ATARAXI': 'Port Hedland', 'RIGEL': 'Kurasawa', 'BARNARD': 'Rostov', 'WOLF359': "Hubble's Star",
       'SHMRN': 'Venice', 'LUYTEN': "Hell's Kitchen"}
ROOMS = [('briefing', 'Briefing'), ('win', 'Debrief, flight won'), ('lose', 'Debrief, flight lost'), ('bar', 'Bar'),
         ('lounge', 'Lounge'), ('deck', 'Deck'), ('radio', 'Radio at launch')]


def when(conds):
    out = []
    for c in conds:
        k, v = c.split('=', 1)
        if k == 'after':
            out.append('after a %s' % {'won': 'win', 'lost': 'loss'}[v])
        elif k == 'system':
            out.append('system %s' % v)
        elif k == 'dead':
            out.append('%s dead' % NAME.get(v, v))
        elif k == 'alive':
            out.append('%s alive' % NAME.get(v, v))
        elif k == 'escort':
            out.append('escort lost')
        elif k == 'if':
            out.append('flew %s' % SYSTEM.get(v, v))
        elif k == 'unless':
            out.append('never flew %s' % SYSTEM.get(v, v))
    return out


def rows_of(text):
    rooms, seats, wing, wipe = {}, {}, None, None
    for line in text.splitlines():
        key, _, val = line.partition(' ')
        val = val.strip()
        if key == 'ship' and val.startswith('wingman') and 'pilot=' in val:
            wing = NAME.get(val.split('pilot=')[1].split()[0])
        elif key == 'wipe':
            wipe = 'everyone' if val == 'ALL' else NAME.get(val, val)
        elif key in ('bar', 'lounge'):
            seats[key] = [NAME.get(x.split('=')[1], x.split('=')[1]) for x in val.split() if x.split('=')[1] != '-']
        elif key in ('briefing', 'win', 'lose', 'talk', 'say', 'radio'):
            if key in ('briefing', 'win', 'lose') and '"' not in val:
                rooms.setdefault(key, []).append(('Solberg', val, [], None, None))
                continue
            f = shlex.split(val)
            line_, words = f[-1], f[:-1]
            conds = [w for w in words if '=' in w and not w.startswith('boost=')]
            boost = next((w[6:] for w in words if w.startswith('boost=')), None)
            pos = [w for w in words if '=' not in w]
            if key in ('briefing', 'win', 'lose'):
                rooms.setdefault(key, []).append((NAME.get(pos[0], pos[0]) if pos else 'Solberg', line_, when(conds), None, boost))
            elif key == 'radio':
                rooms.setdefault('radio', []).append((wing or 'Wingman', line_, when(conds), None, None))
            elif key == 'talk':
                rooms.setdefault(pos[0], []).append((NAME[pos[1]], line_, when(conds), 'as you walk in' if pos[0] == 'deck' else 'table talk', None))
            else:
                rooms.setdefault(pos[0], []).append((NAME.get(pos[2], pos[2]), line_, when(conds), 'click ' + NAME.get(pos[1], pos[1]), boost))
    return rooms, seats, wing, wipe


def main(out):
    missions = re.split(r'(?m)^(?=mission  )', open(os.path.join(ROOT, 'data', 'missions.txt')).read())
    by_id, total = {}, 0
    for m in missions:
        h = re.match(r'mission\s+(\S+)', m)
        if h:
            by_id[h.group(1)] = rows_of(m)
            total += sum(len(v) for v in by_id[h.group(1)][0].values())
    camp = list(csv.DictReader(open(os.path.join(ROOT, 'data', 'campaign.csv'))))
    e = html.escape
    body, nav, tree = [], [], []
    for c in camp:
        node, ids = c['node'], c['missions'].split()
        wings = by_id[ids[0]][2] or '-'
        nav.append('<a href="#%s">%s</a>' % (node, e(SYSTEM[node])))
        good, bad = SYSTEM.get(c['good'], c['good']), SYSTEM.get(c['bad'], c['bad'])
        tree.append('<tr><th><a href="#%s">%s</a></th><td>%s</td><td>%d</td><td>%s</td><td>%s</td><td>%s</td></tr>' % (
            node, e(SYSTEM[node]), e(wings), len(ids), e(OLD[node]), e(good), e(bad)))
        body.append('<section class="system" id="%s"><h2>%s</h2><p class="meta">Wingman %s. Follows the old game\'s %s. '
                    'Won: on to %s. Lost: on to %s.</p>' % (node, e(SYSTEM[node]), e(wings), e(OLD[node]), e(good), e(bad)))
        for i, mid in enumerate(ids, 1):
            rooms, seats, wing, wipe = by_id[mid]
            body.append('<article class="mission"><h3>Mission %d of %d</h3>' % (i, len(ids)))
            if wipe:
                body.append('<p class="meta">Tally wipes the board for %s as this mission begins.</p>' % e(wipe))
            for key, label in ROOMS:
                if key not in rooms:
                    continue
                sat = ''
                if key in seats:
                    sat = ' <span class="seated">%s</span>' % e(', '.join(seats[key]) or 'nobody at the table')
                body.append('<div class="room"><h4>%s%s</h4><div class="lines">' % (e(label), sat))
                for who, line, conds, how, boost in rooms[key]:
                    tags = ([how] if how else []) + conds
                    chips = ''.join('<span class="chip">%s</span>' % e(t) for t in tags)
                    if boost:
                        chips += '<span class="chip boost">gives %s</span>' % e(boost)
                    body.append('<div class="line" data-who="%s"><span class="who">%s</span><span class="said">%s</span>'
                                '<span class="when">%s</span></div>' % (e(who), e(who), e(line), chips))
                body.append('</div></div>')
            body.append('</article>')
        body.append('</section>')
    people = ['Solberg', 'Wildcard', 'Frost', 'Drifter', 'Comet', 'Anchor', 'Prime', 'Echo', 'Kestrel', 'Kit', 'Torch', 'Tally']
    chips = ''.join('<button type="button" class="pick" data-p="%s" aria-pressed="false">%s</button>' % (p, p) for p in people)
    page = TEMPLATE.replace('@TOTAL@', '{:,}'.format(total)).replace('@MISSIONS@', str(len(by_id))).replace('@TREE@', ''.join(tree)) \
        .replace('@NAV@', ''.join(nav)).replace('@PEOPLE@', chips).replace('@BODY@', ''.join(body))
    open(out, 'w').write(page)
    print('%s: %d missions, %d lines, %d KB' % (out, len(by_id), total, len(page) // 1024))


TEMPLATE = r'''<title>The RetroVega Script</title>
<meta name="claude:viewport-fit-sides" content="cover">
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Courier+Prime:wght@400;700&family=Archivo+Narrow:wght@500;700&display=swap">
<style>
/* A shooting script in a flight ops binder: typewriter lines, stencilled headings, the cockpit's amber for cues.
   Layout: a left rail of systems beside one reading column; the rail folds away on a phone. */
:root {
  --bg: #f3f2ed; --panel: #e8e6dd; --fg: #1f2226; --muted: #5d6168; --rule: #cfccc0;
  --cue: #8a4b00; --chip: #dedbd0; --boost: #1f5f5b; --boost-fg: #f3f2ed; --link: #1f5f5b;
  --type: "Courier Prime", "Courier New", monospace; --head: "Archivo Narrow", "Arial Narrow", sans-serif;
}
@media (prefers-color-scheme: dark) { :root:not([data-theme="light"]) {
  --bg: #15181c; --panel: #1d2126; --fg: #e4e2da; --muted: #9a9d9f; --rule: #30353b;
  --cue: #e5a64a; --chip: #2a2f35; --boost: #7fd0c8; --boost-fg: #10201f; --link: #7fd0c8; color-scheme: dark } }
:root[data-theme="dark"] {
  --bg: #15181c; --panel: #1d2126; --fg: #e4e2da; --muted: #9a9d9f; --rule: #30353b;
  --cue: #e5a64a; --chip: #2a2f35; --boost: #7fd0c8; --boost-fg: #10201f; --link: #7fd0c8; color-scheme: dark }
body { background: var(--bg); color: var(--fg); font: 15px/1.5 var(--type) }
a { color: var(--link) }
.wrap { display: grid; grid-template-columns: 13rem minmax(0, 1fr); gap: 2rem; max-width: 66rem; margin: 0 auto;
  padding-block: 1.5rem 4rem; padding-left: max(16px, env(safe-area-inset-left, 0px)); padding-right: max(16px, env(safe-area-inset-right, 0px)) }
nav { position: sticky; top: env(safe-area-inset-top, 0px); align-self: start; max-height: 100vh; overflow-y: auto; display: flex; flex-direction: column; gap: .15rem; padding-block: .5rem; font-family: var(--head); font-size: .95rem }
nav a { text-decoration: none; padding: .15rem .4rem; border-radius: 3px }
nav a:hover, nav a:focus-visible { background: var(--panel) }
h1, h2, h3, h4 { font-family: var(--head); text-wrap: balance; margin: 0 }
h1 { font-size: 2.1rem; letter-spacing: .02em; text-transform: uppercase }
h2 { font-size: 1.5rem; letter-spacing: .03em; text-transform: uppercase; border-top: 3px solid var(--fg); padding-top: .5rem; scroll-margin-top: 4rem }
h3 { font-size: 1.05rem; letter-spacing: .08em; text-transform: uppercase; color: var(--muted) }
h4 { font-size: .95rem; letter-spacing: .04em; color: var(--cue); display: flex; flex-wrap: wrap; gap: .5rem; align-items: baseline }
.seated { color: var(--muted); font-weight: 500; letter-spacing: 0 }
main { display: flex; flex-direction: column; gap: 2.5rem; min-width: 0 }
.lead { max-width: 65ch; display: flex; flex-direction: column; gap: .75rem }
.lead p, .meta { margin: 0 }
.meta { color: var(--muted); font-size: .9rem; max-width: 65ch }
.scroll { overflow-x: auto }
table { border-collapse: collapse; font-family: var(--head); font-size: .95rem; width: 100%; min-width: 34rem }
th, td { text-align: left; padding: .25rem .6rem .25rem 0; border-bottom: 1px solid var(--rule); font-weight: 500; vertical-align: top }
thead th { color: var(--muted); font-weight: 700 }
.notes { display: flex; flex-direction: column; gap: .35rem; margin: 0; padding-left: 1.1rem; max-width: 65ch }
.tools { position: sticky; top: env(safe-area-inset-top, 0px); z-index: 5; background: var(--bg); padding-block: .6rem; border-bottom: 1px solid var(--rule); display: flex; flex-wrap: wrap; gap: .35rem; align-items: center }
.tools label { font-family: var(--head); color: var(--muted); margin-right: .25rem }
.tools input { font: inherit; background: var(--panel); color: var(--fg); border: 1px solid var(--rule); border-radius: 3px; padding: .2rem .45rem; width: 11rem; max-width: 100% }
.pick { font: 500 .9rem var(--head); background: var(--panel); color: var(--fg); border: 1px solid var(--rule); border-radius: 3px; padding: .1rem .5rem; cursor: pointer }
.pick[aria-pressed="true"] { background: var(--cue); color: var(--bg); border-color: var(--cue) }
button:focus-visible, input:focus-visible, a:focus-visible { outline: 2px solid var(--link); outline-offset: 1px }
#count { font-family: var(--head); color: var(--muted); margin-left: auto }
.system { display: flex; flex-direction: column; gap: 1rem }
.mission { display: flex; flex-direction: column; gap: .9rem }
.room { display: flex; flex-direction: column; gap: .3rem }
.lines { display: flex; flex-direction: column }
.line { display: grid; grid-template-columns: 6rem minmax(0, 1fr); gap: 0 .75rem; padding-block: .12rem }
.who { font-weight: 700; color: var(--cue); font-size: .85rem; text-transform: uppercase; padding-top: .1rem }
.when { grid-column: 2; display: flex; flex-wrap: wrap; gap: .25rem }
.when:empty { display: none }
.chip { font: 500 .78rem var(--head); background: var(--chip); color: var(--muted); padding: 0 .35rem; border-radius: 2px }
.chip.boost { background: var(--boost); color: var(--boost-fg) }
.hide { display: none }
@media (max-width: 720px) { .wrap { grid-template-columns: minmax(0, 1fr) } nav { position: static; max-height: none; flex-direction: row; flex-wrap: wrap } .line { grid-template-columns: minmax(0, 1fr) } .when { grid-column: 1 } }
@media (prefers-reduced-motion: no-preference) { html { scroll-behavior: smooth } }
</style>
<div class="wrap">
<nav aria-label="Systems">@NAV@</nav>
<main>
<header class="lead">
<h1>The RetroVega Script</h1>
<p>All @MISSIONS@ missions and @TOTAL@ lines, exactly as they are in the game. This page is built from the game's data file. Comment on any line and I'll change it.</p>
<p>Each briefing and debrief loosely follows the old game's mission for that system, in new words and in Aera-war terms.</p>
</header>
<section class="lead" aria-label="Choices to check">
<h2>Check these</h2>
<ul class="notes">
<li><b>Names I invented:</b> Sixty-Four the Aera ace, Ensign Tamsin Rowe, the Thermopylae, Harrow fever, Tomas, the Issan family, and Ines Ortega, Torch's sister.</li>
<li><b>Sixty-Four is a story character only:</b> no ship in a flight is marked as the ace, even when the lines say it was there.</li>
<li><b>Kit's boosts:</b> she picks one per mission, as in the arcs doc. There are none at Wolf 359, because she is flying. If you lost the mission before Cephid, she has nothing for you there.</li>
<li><b>Story deaths:</b> Anchor and Drifter die only if Wolf 359 comes out lost, and Tally's last flight ends with him alive unless you lose him.</li>
<li><b>The ending screen</b> is still only YOU WIN or YOU LOSE. The last debriefs carry the ending for now.</li>
<li><b>Tested:</b> every line passes the data checks, including ship counts against the mission. I walked Enigma, Wolf 359 falling into Luyten, and the lost-pilot cases on screen, not every conversation.</li>
</ul>
</section>
<section aria-label="Who flies when">
<h2>The campaign</h2>
<div class="scroll"><table><thead><tr><th>System</th><th>Wingman</th><th>Missions</th><th>Old game</th><th>If won</th><th>If lost</th></tr></thead><tbody>@TREE@</tbody></table></div>
</section>
<div class="tools" role="group" aria-label="Filter the lines">
<label for="find">Find</label><input id="find" type="search" placeholder="a word in a line">
<label>Who</label>@PEOPLE@<span id="count" aria-live="polite"></span>
</div>
@BODY@
</main>
</div>
<script>
(function () {
  var lines = Array.prototype.slice.call(document.querySelectorAll('.line'));
  var rooms = Array.prototype.slice.call(document.querySelectorAll('.room'));
  var missions = Array.prototype.slice.call(document.querySelectorAll('.mission'));
  var systems = Array.prototype.slice.call(document.querySelectorAll('.system'));
  var picks = Array.prototype.slice.call(document.querySelectorAll('.pick'));
  var find = document.getElementById('find'), count = document.getElementById('count');
  var who = '';
  function shown(el, sel) { return !!el.querySelector(sel + ':not(.hide)'); }
  function apply() {
    var q = find.value.trim().toLowerCase(), n = 0;
    lines.forEach(function (l) {
      var on = (!who || l.getAttribute('data-who') === who) && (!q || l.textContent.toLowerCase().indexOf(q) >= 0);
      l.classList.toggle('hide', !on);
      if (on) n++;
    });
    rooms.forEach(function (r) { r.classList.toggle('hide', !shown(r, '.line')); });
    missions.forEach(function (m) { m.classList.toggle('hide', !shown(m, '.room')); });
    systems.forEach(function (s) { s.classList.toggle('hide', !shown(s, '.mission')); });
    count.textContent = n.toLocaleString() + ' lines';
  }
  picks.forEach(function (b) {
    b.addEventListener('click', function () {
      who = who === b.getAttribute('data-p') ? '' : b.getAttribute('data-p');
      picks.forEach(function (p) { p.setAttribute('aria-pressed', p.getAttribute('data-p') === who ? 'true' : 'false'); });
      apply();
    });
  });
  find.addEventListener('input', apply);
  apply();
})();
</script>
'''

if __name__ == '__main__':
    main(sys.argv[1])
