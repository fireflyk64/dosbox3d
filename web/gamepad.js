// Game controllers for the browser build.
//
// A controller is presented to the game as its mouse and its keyboard: the
// sticks set the pointer (Wing Commander steers by how far the pointer is
// from a neutral point of the cockpit view), the buttons press keys.  The
// page picks one controller per window, so two windows side by side can each
// take their own (or one a controller and the other the keyboard and mouse).
//
// The mapping is a table of game actions, each bound to a button (or an axis
// used as a button), plus four analog inputs each bound to an axis that can
// be inverted.  It defaults to the standard gamepad layout and is kept in
// local storage; the choice of controller is per window (session storage).
//
// The emulator side is four exports (src/cpu/wcnet_hooks.cpp): wc_web_key,
// wc_web_pointer, wc_web_mouse_button and wc_web_in_flight.

// DOSBox's KBD_KEYS numbering (include/keyboard.h; a static_assert next to
// wc_web_key checks the order).
export const KEY = (() => {
  const k = {};
  let n = 1;
  for (const c of "1234567890" + "qwertyuiop" + "asdfghjklz" + "xcvbnm") k[c] = n++;
  for (let i = 1; i <= 12; i++) k["f" + i] = n++;
  for (const name of ["esc", "tab", "backspace", "enter", "space", "leftalt", "rightalt", "leftctrl", "rightctrl",
                      "leftshift", "rightshift", "capslock", "scrolllock", "numlock", "grave", "minus", "equals",
                      "backslash", "leftbracket", "rightbracket", "semicolon", "quote", "period", "comma", "slash",
                      "extra_lt_gt", "printscreen", "pause", "insert", "home", "pageup", "delete", "end", "pagedown",
                      "left", "up", "down", "right"]) k[name] = n++;
  return k;
})();

// What a button can do: the keys it holds down, and whether it is also the
// pointer's click outside flight (menus, the barracks).
export const ACTIONS = [
  { id: "guns", label: "Fire guns", keys: ["space"], click: true, bind: { button: 0 } },
  { id: "missile", label: "Fire missile", keys: ["enter"], bind: { button: 1 } },
  { id: "target", label: "Change target", keys: ["t"], bind: { button: 2 } },
  { id: "weapon", label: "Change weapon", keys: ["w"], bind: { button: 3 } },
  { id: "faster", label: "Speed up", keys: ["equals"], bind: { button: 4 } },
  { id: "slower", label: "Slow down", keys: ["minus"], bind: { button: 5 } },
  { id: "burner", label: "Afterburner", keys: ["tab"], bind: { button: 6 }, analog: true },
  { id: "nav", label: "Navigation map", keys: ["n"], bind: { button: 8 } },
  { id: "auto", label: "Autopilot", keys: ["a"], bind: { button: 9 } },
  // (A drone riding behind the leader is the leader's copilot: the Up and
  // Down keys shift the leader's shields, Fire guns puts shields into the
  // guns, Fire missile guns into the weakest shield, Speed up and Slow down
  // set the cruising speed.  For a pilot the two arrows are the keyboard's
  // nose up and down.)
  { id: "up", label: "Up (copilot: shields to the rear)", keys: ["up"], bind: { button: 12 } },
  { id: "down", label: "Down (copilot: shields to the front)", keys: ["down"], bind: { button: 13 } },
  { id: "gunsel", label: "Change guns", keys: ["g"], bind: null },
  { id: "lock", label: "Lock target", keys: ["l"], bind: null },
  { id: "comms", label: "Communications", keys: ["c"], bind: null },
  { id: "esc", label: "Esc (skip a scene)", keys: ["esc"], bind: null },
  // (No key: the page's own action, voice push to talk, through host.onAction.)
  { id: "ptt", label: "Push to talk (voice)", keys: [], bind: { button: 10 } },
];
// Analog inputs.  Not inverted means: stick right turns and rolls right, and
// pushing the stick up raises the nose; pitch is inverted by default, the
// way a flight stick works (pull back to climb).
export const AXES = [
  { id: "yaw", label: "Turn left / right", bind: { axis: 0, invert: false } },
  { id: "pitch", label: "Pitch", bind: { axis: 1, invert: true } },
  { id: "pitch2", label: "Pitch, second stick", bind: { axis: 3, invert: true } },
  { id: "roll", label: "Roll", bind: { axis: 2, invert: false } },
];
const ROLL_KEYS = { left: "comma", right: "period" };
const BUTTON_NAMES = ["A", "B", "X", "Y", "L1", "R1", "L2 (left trigger)", "R2 (right trigger)", "Back", "Start",
                      "L3 (left stick)", "R3 (right stick)", "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Home"];
const AXIS_NAMES = ["Left stick, sideways", "Left stick, up/down", "Right stick, sideways", "Right stick, up/down"];
const CONFIG_KEY = "wcpad:config";
const CHOICE_KEY = "wcpad:choice";
const NONE = "none";

// The stick's sensitivity is the share of the game's full turn that full
// stick asks for: 100%, the stick being analog anyway (it was 70%, which
// stopped at step 5 of the game's 8).  Version 2 of the saved settings:
// a sensitivity saved by version 1 was the old default, not a choice.
export function defaultConfig() {
  const cfg = { version: 2, buttons: {}, axes: {}, deadzone: 0.15, burner: 0.9, sensitivity: 1.0 };
  for (const a of ACTIONS) cfg.buttons[a.id] = a.bind ? { ...a.bind } : null;
  for (const a of AXES) cfg.axes[a.id] = { ...a.bind };
  return cfg;
}
function loadConfig() {
  const cfg = defaultConfig();
  try {
    const saved = JSON.parse(localStorage.getItem(CONFIG_KEY) || "null");
    if (saved && (saved.version === 1 || saved.version === 2)) {
      for (const a of ACTIONS) if (a.id in (saved.buttons || {})) cfg.buttons[a.id] = saved.buttons[a.id];
      for (const a of AXES) if (saved.axes && saved.axes[a.id]) cfg.axes[a.id] = saved.axes[a.id];
      for (const k of saved.version === 1 ? ["deadzone", "burner"] : ["deadzone", "burner", "sensitivity"]) if (typeof saved[k] === "number") cfg[k] = saved[k];
    }
  } catch (e) { /* defaults */ }
  return cfg;
}
function saveConfig(cfg) {
  try { localStorage.setItem(CONFIG_KEY, JSON.stringify(cfg)); } catch (e) { /* not kept, still used */ }
}

const padKey = (pad) => `${pad.index}:${pad.id}`;
const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);
function pads() {
  try { return Array.from(navigator.getGamepads ? navigator.getGamepads() : []).filter((p) => p && p.connected); }
  catch (e) { return []; }
}

// Wing Commander II turns in steps by how far the pointer is from the middle
// of its view: step 1, 2, ... begins at steps[0], steps[1], ... and the last
// and fastest, worth `top`, is the view's edge (`reach` from the middle,
// taken from `edge` before it; a small view has no room for the later
// steps).  The stick asks for a share v (-1..1) of the full turn and gets
// the step nearest to it, with the pointer in the middle of that step's
// band: a stick at rest is no turn at all in any cockpit, and the same
// stick is the same turn in a cockpit with a large window and a small one.
export function stepped(v, steps, reach, edge, top) {
  const inside = steps.filter((from) => from < reach - edge).length;
  const worth = (n) => (n > inside ? top : n);     // 0: no turn, 1..inside, inside + 1: the edge
  const want = Math.abs(v) * top;
  let best = 0;
  for (let n = 1; n <= inside + 1; n++) if (Math.abs(worth(n) - want) < Math.abs(worth(best) - want)) best = n;
  if (best === 0) return 0;
  if (best > inside) return Math.sign(v) * reach;
  const from = steps[best - 1];
  const to = Math.min(reach - edge, best < steps.length ? steps[best] : from + 2 * (from - (steps[best - 2] || 0)));
  return Math.sign(v) * (from + to) / 2;
}

// initControls wires the controller picker and the mapping table (elements
// by id, see index.html) and starts polling.  `host` supplies:
//   module():  the running emulator's Module, or null
//   pointer(): the game's steering pointer {x, y, rx, ry} as fractions of the
//              mouse range (neutral point and reach), or null for the centre;
//              with stepsX / stepsY / edgeX / edgeY / top for a game that
//              turns in steps (see stepped below)
//   log(line)
//   onAction(id, down): an action of the page's own (no key), such as voice
//              push to talk, pressed or released (optional)
export function initControls(host) {
  const $ = (id) => document.getElementById(id);
  let cfg = loadConfig();
  let choice = null;            // padKey of this window's controller, NONE, or null (nothing chosen yet)
  try { choice = sessionStorage.getItem(CHOICE_KEY); } catch (e) { /* per page load then */ }
  let capture = null;           // { kind: "button" | "axis", id, axes: [...], down: Set } while remapping
  let listed = "";              // the controllers the picker currently shows
  const held = new Map();       // action id -> true while its keys are down
  let rollHeld = null;          // "left" | "right" | null
  let clickHeld = false;
  let lastPointer = { x: -1, y: -1, at: 0 };
  let menu = { x: 0.5, y: 0.5, wasFlight: false, at: 0 };
  const others = new Map();     // padKey -> time another window last claimed it
  let channel = null;
  try {
    channel = new BroadcastChannel("wcpad");
    channel.onmessage = (ev) => {
      const m = ev.data || {};
      if (m.t === "claim" && m.key) others.set(m.key, Date.now());
      else if (m.t === "release" && m.key) others.delete(m.key);
    };
  } catch (e) { /* no coordination between windows */ }
  let lastClaim = 0;

  const selected = () => (choice && choice !== NONE ? pads().find((p) => padKey(p) === choice) || null : null);
  const usedElsewhere = (key) => (Date.now() - (others.get(key) || 0)) < 5000;

  function choose(key, how) {
    if (key === choice) return;
    if (choice && choice !== NONE && channel) channel.postMessage({ t: "release", key: choice });
    releaseAll();
    choice = key;
    try { if (key) sessionStorage.setItem(CHOICE_KEY, key); else sessionStorage.removeItem(CHOICE_KEY); } catch (e) { /* fine */ }
    capture = null;
    lastClaim = 0;
    listed = "";
    if (key && key !== NONE) host.log(`controller: ${key.slice(key.indexOf(":") + 1)} ${how}`);
    render();
  }

  // -- the picker and the mapping table ----------------------------------------

  function bindingText(b) {
    if (!b) return "not set";
    if (b.button !== undefined) return `${BUTTON_NAMES[b.button] || "Button " + b.button}`;
    return `${AXIS_NAMES[b.axis] || "Axis " + b.axis} ${b.dir < 0 ? "−" : "+"}`;
  }
  function render() {
    const sel = $("padSelect");
    const list = pads();
    const signature = list.map(padKey).join("|") + "/" + choice + "/" + list.map((p) => usedElsewhere(padKey(p))).join("");
    if (signature !== listed) {
      listed = signature;
      sel.innerHTML = "";
      const add = (value, text) => { const o = document.createElement("option"); o.value = value; o.textContent = text; sel.appendChild(o); };
      add(NONE, "Keyboard + mouse");
      for (const p of list) add(padKey(p), `${p.index + 1}: ${p.id}${usedElsewhere(padKey(p)) ? " (in use in another window)" : ""}`);
      if (choice && choice !== NONE && !list.some((p) => padKey(p) === choice)) add(choice, `${choice.slice(choice.indexOf(":") + 1)} (not connected)`);
      sel.value = choice || NONE;
    }
    const pad = selected();
    $("padHint").textContent = pad
      ? (pad.mapping === "standard" ? "Using the standard gamepad layout; change any button or axis below."
                                    : "This controller does not report the standard layout: check the buttons and axes below.")
      : (list.length ? "Pick a controller, or press one of its buttons while this window has the focus."
                     : "No controller seen yet: plug one in and press one of its buttons.");

    const rows = [];
    for (const a of AXES) {
      const b = cfg.axes[a.id];
      const busy = capture && capture.kind === "axis" && capture.id === a.id;
      rows.push(`<tr><td>${a.label}</td><td>${busy ? "move that stick…" : (AXIS_NAMES[b.axis] || "Axis " + b.axis)}</td>` +
                `<td><label class="check"><input type="checkbox" data-invert="${a.id}"${b.invert ? " checked" : ""}> invert</label></td>` +
                `<td><button type="button" class="secondary" data-axis="${a.id}">${busy ? "Cancel" : "Change"}</button></td></tr>`);
    }
    for (const a of ACTIONS) {
      const b = cfg.buttons[a.id];
      const busy = capture && capture.kind === "button" && capture.id === a.id;
      rows.push(`<tr><td>${a.label}</td><td>${busy ? "press a button…" : bindingText(b)}</td><td></td>` +
                `<td><button type="button" class="secondary" data-button="${a.id}">${busy ? "Cancel" : "Change"}</button>` +
                (b && !busy ? ` <button type="button" class="secondary" data-clear="${a.id}">Clear</button>` : "") + `</td></tr>`);
    }
    $("padRows").innerHTML = rows.join("");
    for (const [id, key, scale] of [["padSensitivity", "sensitivity", 100], ["padDeadzone", "deadzone", 100], ["padBurner", "burner", 100]]) {
      $(id).value = String(Math.round(cfg[key] * scale));
      $(id + "Value").textContent = `${Math.round(cfg[key] * scale)}%`;
    }
  }

  $("padSelect").addEventListener("change", (e) => choose(e.target.value, "selected"));
  $("padRows").addEventListener("click", (e) => {
    const t = e.target;
    if (!(t instanceof HTMLElement)) return;
    const start = (kind, id) => {
      const pad = selected();
      if (capture && capture.kind === kind && capture.id === id) capture = null;
      else if (pad) capture = { kind, id, axes: Array.from(pad.axes), down: new Set(pad.buttons.map((b, i) => (b.pressed ? i : -1)).filter((i) => i >= 0)) };
      render();
    };
    if (t.dataset.axis) start("axis", t.dataset.axis);
    else if (t.dataset.button) start("button", t.dataset.button);
    else if (t.dataset.clear) { cfg.buttons[t.dataset.clear] = null; saveConfig(cfg); releaseAll(); render(); }
  });
  $("padRows").addEventListener("change", (e) => {
    const t = e.target;
    if (t instanceof HTMLInputElement && t.dataset.invert) { cfg.axes[t.dataset.invert].invert = t.checked; saveConfig(cfg); }
  });
  for (const [id, key] of [["padSensitivity", "sensitivity"], ["padDeadzone", "deadzone"], ["padBurner", "burner"]]) {
    $(id).addEventListener("input", (e) => { cfg[key] = Number(e.target.value) / 100; $(id + "Value").textContent = `${e.target.value}%`; saveConfig(cfg); });
  }
  $("padReset").addEventListener("click", () => { cfg = defaultConfig(); saveConfig(cfg); capture = null; releaseAll(); render(); });

  // -- reading the controller ---------------------------------------------------

  const shaped = (v) => {                      // dead zone, then the rest of the travel rescaled to 0..1
    const d = cfg.deadzone;
    const m = Math.abs(v);
    return m <= d ? 0 : Math.sign(v) * (m - d) / (1 - d);
  };
  const axisValue = (pad, id) => {
    const b = cfg.axes[id];
    const v = shaped(pad.axes[b.axis] || 0);
    return b.invert ? -v : v;
  };
  function buttonValue(pad, b) {              // 0..1: how far the bound button (or axis) is pressed
    if (!b) return 0;
    if (b.button !== undefined) { const x = pad.buttons[b.button]; return x ? (x.value || (x.pressed ? 1 : 0)) : 0; }
    return Math.max(0, (pad.axes[b.axis] || 0) * (b.dir < 0 ? -1 : 1));
  }

  function sendKeys(M, names, down) { for (const n of names) M._wc_web_key(KEY[n], down ? 1 : 0); }
  function releaseAll() {
    const M = host.module();
    if (M && M._wc_web_key) {
      for (const a of ACTIONS) if (held.get(a.id)) sendKeys(M, a.keys, false);
      if (rollHeld) sendKeys(M, [ROLL_KEYS[rollHeld]], false);
      if (clickHeld) M._wc_web_mouse_button(0, 0);
    }
    held.clear(); rollHeld = null; clickHeld = false;
  }

  function capturing(pad) {
    if (capture.kind === "button") {
      const i = pad.buttons.findIndex((b, n) => b.pressed && !capture.down.has(n));
      if (i >= 0) cfg.buttons[capture.id] = { button: i };
      else {
        const a = pad.axes.findIndex((v, n) => Math.abs(v - (capture.axes[n] || 0)) > 0.6);
        if (a < 0) { for (const n of Array.from(capture.down)) if (!pad.buttons[n] || !pad.buttons[n].pressed) capture.down.delete(n); return; }
        cfg.buttons[capture.id] = { axis: a, dir: pad.axes[a] - (capture.axes[a] || 0) < 0 ? -1 : 1 };
      }
    } else {
      const a = pad.axes.findIndex((v, n) => Math.abs(v - (capture.axes[n] || 0)) > 0.6);
      if (a < 0) return;
      cfg.axes[capture.id].axis = a;
    }
    capture = null;
    saveConfig(cfg);
    render();
  }

  function drive(pad, M, now) {
    const flying = M._wc_web_in_flight() !== 0;
    const yaw = axisValue(pad, "yaw");
    // Pitch: positive raises the nose.  The game turns towards its pointer,
    // so the nose comes up when the pointer is above the neutral point
    // (checked on screen against the carrier, not just in the game's vectors).
    const pitchOf = (id) => { const b = cfg.axes[id]; const v = shaped(pad.axes[b.axis] || 0); return b.invert ? v : -v; };
    const pitch = clamp(pitchOf("pitch") + pitchOf("pitch2"), -1, 1);
    const roll = axisValue(pad, "roll");
    let x, y, refresh;
    if (flying) {
      const p = host.pointer() || { x: 0.5, y: 0.5, rx: 0.5, ry: 0.5 };
      if (p.stepsX) {
        x = p.x + stepped(yaw * cfg.sensitivity, p.stepsX, p.rx, p.edgeX, p.top);
        y = p.y - stepped(pitch * cfg.sensitivity, p.stepsY, p.ry, p.edgeY, p.top);
      } else {
        x = p.x + yaw * p.rx * cfg.sensitivity;
        y = p.y - pitch * p.ry * cfg.sensitivity;
      }
      refresh = 200;             // the game re-centres the pointer by itself now and then
      menu.wasFlight = true;
    } else {
      // Menus and the barracks: the stick moves the pointer like a mouse.
      if (menu.wasFlight) { menu.x = 0.5; menu.y = 0.5; menu.wasFlight = false; }
      const dt = Math.min(0.1, (now - (menu.at || now)) / 1000);
      const rawX = shaped(pad.axes[cfg.axes.yaw.axis] || 0);
      const rawY = clamp(shaped(pad.axes[cfg.axes.pitch.axis] || 0) + shaped(pad.axes[cfg.axes.pitch2.axis] || 0), -1, 1);
      menu.x = clamp(menu.x + rawX * 0.6 * dt, 0, 1);
      menu.y = clamp(menu.y + rawY * 0.6 * dt, 0, 1);
      x = menu.x; y = menu.y; refresh = 1e9;
    }
    menu.at = now;
    if (Math.abs(x - lastPointer.x) > 0.001 || Math.abs(y - lastPointer.y) > 0.001 || now - lastPointer.at > refresh) {
      M._wc_web_pointer(x, y);
      lastPointer = { x, y, at: now };
    }
    const wantRoll = flying && Math.abs(roll) > 0.5 ? (roll < 0 ? "left" : "right") : null;
    if (wantRoll !== rollHeld) {
      if (rollHeld) sendKeys(M, [ROLL_KEYS[rollHeld]], false);
      if (wantRoll) sendKeys(M, [ROLL_KEYS[wantRoll]], true);
      rollHeld = wantRoll;
    }
    for (const a of ACTIONS) {
      const v = buttonValue(pad, cfg.buttons[a.id]);
      // An analog trigger has to be nearly all the way down (the afterburner
      // drinks fuel); letting go needs a little less, so it does not flutter.
      const on = a.analog ? cfg.burner : 0.5;
      const down = held.get(a.id) ? v > on - 0.1 : v >= on;
      if (down !== !!held.get(a.id)) {
        if (a.keys.length) sendKeys(M, a.keys, down); else if (host.onAction) host.onAction(a.id, down);
        held.set(a.id, down);
      }
      if (a.click) {
        const click = down && !flying;
        if (click !== clickHeld) { M._wc_web_mouse_button(0, click ? 1 : 0); clickHeld = click; }
      }
    }
  }

  function tick() {
    const now = performance.now();
    const list = pads();
    // Nothing chosen in this window yet: the first controller that presses a
    // button while the window has the focus becomes its controller.
    if (choice === null && document.hasFocus()) {
      const p = list.find((q) => !usedElsewhere(padKey(q)) && q.buttons.some((b) => b.pressed));
      if (p) choose(padKey(p), "picked up from its button press");
    }
    const pad = selected();
    if (channel && choice && choice !== NONE && pad && now - lastClaim > 2000) { channel.postMessage({ t: "claim", key: choice }); lastClaim = now; }
    const M = host.module();
    if (pad && capture) capturing(pad);
    else if (pad && M && M._wc_web_key && !capture) drive(pad, M, now);
    else if (held.size || rollHeld || clickHeld) releaseAll();
    if (list.map(padKey).join("|") + "/" + choice + "/" + list.map((p) => usedElsewhere(padKey(p))).join("") !== listed) render();
  }
  window.addEventListener("gamepadconnected", () => render());
  window.addEventListener("gamepaddisconnected", () => { releaseAll(); render(); });
  window.addEventListener("pagehide", () => { if (channel && choice && choice !== NONE) channel.postMessage({ t: "release", key: choice }); });
  render();
  // A timer rather than animation frames: a window that is visible but not
  // focused (the other half of a split screen) must keep reading its controller.
  setInterval(tick, 16);
  return { config: () => cfg, choice: () => choice };
}
