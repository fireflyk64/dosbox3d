// Two-handed touch flight controls, shared by DOSBox and the native browser build. The left stick moves the
// game's steering pointer without clicking; the right buttons send keys. Outside flight, canvas taps still click.
//
// The keyboard is a hidden text field in #screen: focusing it is what makes a phone show its keyboard.  What it
// types reaches the game as key presses through the page's send(name, down), by gamepad.js's KEY names (the
// emulator's queue, wc_web_key, on the DOSBox page; the PC's scan codes on RetroVega's own).  Phone keyboards compose words and correct them, and say little in their key events (Android's are mostly
// "Unidentified"), so the field's value is followed instead: each change is sent as the backspaces and the letters
// that turn what was typed into what is there now.  Enter, Esc, Tab and the arrows come as key events and are sent as
// they are; none of the field's key events reach the emulator's own listener (it would type them twice).
// A character as the key that types it on a US keyboard, and whether Shift is held for it.
const SHIFTED = { "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7", "*": "8", "(": "9", ")": "0",
                  "_": "minus", "+": "equals", "?": "slash", ":": "semicolon", '"': "quote", "<": "comma",
                  ">": "period", "{": "leftbracket", "}": "rightbracket", "|": "backslash", "~": "grave" };
const PLAIN = { " ": "space", "-": "minus", "=": "equals", ".": "period", ",": "comma", "/": "slash", ";": "semicolon",
                "'": "quote", "[": "leftbracket", "]": "rightbracket", "\\": "backslash", "`": "grave" };
function keyOf(ch) {
  if (/^[a-z0-9]$/.test(ch)) return { name: ch, shift: false };
  if (/^[A-Z]$/.test(ch)) return { name: ch.toLowerCase(), shift: true };
  if (PLAIN[ch]) return { name: PLAIN[ch], shift: false };
  if (SHIFTED[ch]) return { name: SHIFTED[ch], shift: true };
  return null;                          // (é, emoji: the game's keyboard has none)
}
const NAMED = { Enter: "enter", Escape: "esc", Tab: "tab", ArrowLeft: "left", ArrowRight: "right", ArrowUp: "up",
                ArrowDown: "down", Delete: "delete" };

// send(name, down): a key by its KEY name to the game, false when no game takes keys (what is waiting is dropped)
export function initTouch({ send: sendKey, log, inFlight = () => false, steer = () => false }) {
  const bar = document.getElementById("touch");
  const field = document.getElementById("touchType");
  const keysBtn = document.getElementById("touchKeys");
  const stick = document.getElementById("touchStick");
  const knob = document.getElementById("touchKnob");
  const touchy = () => navigator.maxTouchPoints > 0 || matchMedia("(pointer: coarse)").matches;

  // the keys go out a few at a time: DOSBox's keyboard holds 32 codes, and a word with capitals is many
  const queue = [];
  const downKeys = new Set();
  let timer = null;
  function transmit(name, down) {
    if (sendKey(name, down) === false) return false;
    if (down) downKeys.add(name); else downKeys.delete(name);
    return true;
  }
  function pump() {
    timer = null;
    for (let i = 0; i < 6 && queue.length; i++) {
      const [name, down] = queue.shift();
      if (!transmit(name, down)) { queue.length = 0; break; }
    }
    if (queue.length) timer = setTimeout(pump, 30);
  }
  function send(name, down) {
    queue.push([name, down ? 1 : 0]);
    if (timer === null) pump();
  }
  function tap(name, shift) {
    if (shift) send("leftshift", true);
    send(name, true);
    send(name, false);
    if (shift) send("leftshift", false);
  }
  function type(text) { for (const ch of text) { const k = keyOf(ch); if (k) tap(k.name, k.shift); } }

  // the field: what has been sent since it was last emptied, against what it holds now
  let sent = "";
  function follow() {
    const now = field.value;
    let p = 0;
    while (p < sent.length && p < now.length && sent[p] === now[p]) p++;
    for (let i = p; i < sent.length; i++) tap("backspace", false);
    type(now.slice(p));
    sent = now;
  }
  function empty() { field.value = ""; sent = ""; }
  field.addEventListener("input", (e) => { e.stopPropagation(); follow(); });
  field.addEventListener("compositionend", () => { if (sent.length > 40) empty(); });
  field.addEventListener("keydown", (e) => {
    e.stopPropagation();
    const named = NAMED[e.key];
    if (named) {
      e.preventDefault();
      if (named === "enter") { follow(); empty(); }
      tap(named, false);
    } else if (e.key === "Backspace" && !field.value) {
      e.preventDefault();               // (nothing in the field to take back: the game's last letter goes)
      tap("backspace", false);
    }
  });
  for (const ev of ["keyup", "keypress"]) field.addEventListener(ev, (e) => e.stopPropagation());
  field.addEventListener("blur", () => { empty(); keysBtn.classList.remove("on"); });
  field.addEventListener("focus", () => keysBtn.classList.add("on"));

  // Keep controls out of SDL's mouse/touch handlers and keep the keyboard's focus when a button is pressed.
  for (const ev of ["pointerdown", "pointermove", "pointerup", "pointercancel", "touchstart", "touchmove",
                    "touchend", "touchcancel", "mousedown", "mousemove", "mouseup", "click"])
    bar.addEventListener(ev, (e) => e.stopPropagation());
  bar.addEventListener("contextmenu", (e) => e.preventDefault());
  for (const b of bar.querySelectorAll("button")) b.addEventListener("pointerdown", (e) => e.preventDefault());
  keysBtn.addEventListener("click", () => {
    if (document.activeElement === field) field.blur();
    else { empty(); field.focus({ preventScroll: true }); }
  });
  document.getElementById("touchEsc").addEventListener("click", () => tap("esc", false));
  for (const [id, name] of Object.entries({ touchAuto: "a", touchTgt: "t", touchMissile: "enter",
    touchComm: "c", touchNav: "n", touchWeapon: "w" }))
    document.getElementById(id).addEventListener("click", () => tap(name, false));

  const holds = new Map();
  for (const [id, name] of Object.entries({ touchFire: "space", touchBurn: "tab", touchSlower: "minus", touchFaster: "equals" })) {
    const button = document.getElementById(id);
    const off = () => {
      if (!holds.has(button)) return;
      const pointer = holds.get(button).pointer;
      holds.delete(button);
      button.classList.remove("on");
      send(name, false);
      if (button.hasPointerCapture(pointer)) button.releasePointerCapture(pointer);
    };
    button.addEventListener("pointerdown", (e) => {
      if (holds.has(button) || e.button !== 0) return;
      holds.set(button, { pointer: e.pointerId, off });
      button.setPointerCapture(e.pointerId);
      button.classList.add("on");
      send(name, true);
    });
    for (const ev of ["pointerup", "pointercancel", "lostpointercapture"])
      button.addEventListener(ev, (e) => { if (holds.get(button)?.pointer === e.pointerId) off(); });
    // Keyboard and assistive-technology activation still make a single press.
    button.addEventListener("click", (e) => { if (e.detail === 0) tap(name, false); });
  }

  let flying = false, stickPointer = null, sx = 0, sy = 0, frame = null, lastSteer = 0;
  function centre() {
    const pointer = stickPointer;
    stickPointer = null;
    sx = sy = 0;
    knob.style.transform = "translate(0px, 0px)";
    stick.classList.remove("on");
    if (flying) steer(0, 0);
    if (pointer !== null && stick.hasPointerCapture(pointer)) stick.releasePointerCapture(pointer);
  }
  function move(e) {
    const r = stick.getBoundingClientRect(), radius = r.width * .36;
    let x = (e.clientX - r.left - r.width / 2) / radius;
    let y = (e.clientY - r.top - r.height / 2) / radius;
    const length = Math.hypot(x, y);
    if (length > 1) { x /= length; y /= length; }
    knob.style.transform = `translate(${x * radius}px, ${y * radius}px)`;
    // A small radial dead zone lets a resting thumb fly straight, with the remaining travel scaled to full turn.
    const strength = Math.max(0, Math.min(1, length) - .12) / .88;
    const unit = Math.hypot(x, y) || 1;
    sx = x / unit * strength;
    sy = y / unit * strength;
    steer(sx, sy);
    lastSteer = performance.now();
  }
  stick.addEventListener("pointerdown", (e) => {
    e.preventDefault();
    if (!flying || stickPointer !== null || e.button !== 0) return;
    stickPointer = e.pointerId;
    stick.setPointerCapture(e.pointerId);
    stick.classList.add("on");
    move(e);
  });
  stick.addEventListener("pointermove", (e) => { if (e.pointerId === stickPointer) move(e); });
  for (const ev of ["pointerup", "pointercancel", "lostpointercapture"])
    stick.addEventListener(ev, (e) => { if (e.pointerId === stickPointer) centre(); });

  function release() {
    // Drop any queued typing too: even a partially transmitted Shift must be released when the page is hidden.
    clearTimeout(timer); timer = null; queue.length = 0;
    for (const { off } of [...holds.values()]) off();
    centre();
    clearTimeout(timer); timer = null; queue.length = 0;
    for (const name of [...downKeys]) transmit(name, 0);
  }
  function update(now) {
    const next = !!inFlight();
    if (next !== flying) {
      release();
      flying = next;
      document.getElementById("touchMovement").hidden = !flying;
      document.getElementById("touchFlight").hidden = !flying;
      bar.classList.toggle("in-flight", flying);
      bar.closest("#screen").classList.toggle("touch-flight", flying);
      if (flying) steer(0, 0);
    }
    // Some games recenter their mouse driver; maintain a held deflection without synthesizing mouse buttons.
    if (flying && stickPointer !== null && now - lastSteer >= 100) { steer(sx, sy); lastSteer = now; }
    frame = requestAnimationFrame(update);
  }
  window.addEventListener("blur", release);
  window.addEventListener("pagehide", release);
  document.addEventListener("visibilitychange", () => { if (document.hidden) release(); });

  // Cockpit touches belong to neither hand's control: don't let SDL turn them into steering or gun clicks.
  // Install before the runtime's listeners. Room/menu taps retain their ordinary mouse behavior.
  let lastCanvasTouch = 0;
  for (const ev of ["pointerdown", "pointermove", "pointerup", "pointercancel", "touchstart", "touchmove",
                    "touchend", "touchcancel", "mousedown", "mousemove", "mouseup"])
    document.addEventListener(ev, (e) => {
      if (bar.hidden || !flying || e.target.id !== "canvas") return;
      const isTouch = e.type.startsWith("touch") || e.pointerType === "touch" || e.sourceCapabilities?.firesTouchEvents;
      if (isTouch) lastCanvasTouch = performance.now();
      if (isTouch || (e.type.startsWith("mouse") && performance.now() - lastCanvasTouch < 800)) {
        e.preventDefault();
        e.stopImmediatePropagation();
      }
    }, { capture: true, passive: false });

  return {
    // while a game runs, on a screen with touch
    show(on) {
      bar.hidden = !(on && touchy());
      if (bar.hidden) { cancelAnimationFrame(frame); frame = null; release(); field.blur(); }
      else if (frame === null) frame = requestAnimationFrame(update);
      if (on && !bar.hidden) log("touch: left thumb steers; right thumb fires, boosts, adjusts throttle, targets and uses AUTO; KEYS types");
    },
  };
}
