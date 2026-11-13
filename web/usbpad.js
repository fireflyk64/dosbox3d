// Xbox 360-style ("XInput") controllers that the browser cannot see on a Mac,
// read by the page itself over WebUSB.
//
// A Logitech F310, F510 or F710 with its switch on X (and the other pads that
// speak the wired Xbox 360 protocol) has no driver on macOS, and Chrome's own
// Xbox support there takes only Microsoft's controllers: navigator.getGamepads()
// never lists it, whatever is pressed.  Linux (xpad) and Windows (XInput) have
// drivers, so there it is an ordinary gamepad and this file is not offered.
//
// Nothing here runs until the player presses "Scan for joysticks" (gamepad.js
// shows the button only where it can help: a Mac with WebUSB).  The scan opens
// the pads the browser was already allowed to (a pad picked at an earlier scan
// opens without asking), and when none is new it shows the browser's chooser.
// Each pad opened is presented as a Gamepad in the standard layout, so the
// mapping table, the dead zone and the rest of gamepad.js treat it as any other.
//
// The pad's input report (20 bytes, sent only when something changes):
//   0: 0x00 (an input report; 0x01 is the LED's state, 0x03 the rumble's), 1: 0x14 (its length),
//   2: d-pad up, down, left, right, Start, Back, left stick click, right stick click (bits 0-7),
//   3: LB, RB, Guide, -, A, B, X, Y (bits 0-7),
//   4: left trigger, 5: right trigger (0-255),
//   6-13: left stick x, y, right stick x, y (signed 16 bits, little-endian, up positive).

const XINPUT = { classCode: 0xff, subclassCode: 0x5d, protocolCode: 0x01 };
// (Logitech's three by name as well: X mode's product numbers.)
const FILTERS = [XINPUT, { vendorId: 0x046d, productId: 0xc21d }, { vendorId: 0x046d, productId: 0xc21e }, { vendorId: 0x046d, productId: 0xc21f }];
// The browser's own pads are 0-3 in getGamepads(); these come after them.
const FIRST_INDEX = 4;
const MAX_PADS = 4;
// Standard layout: the button each bit of bytes 2 and 3 is (-1: none).
const LOW_BITS = [12, 13, 14, 15, 9, 8, 10, 11];
const HIGH_BITS = [4, 5, 16, -1, 0, 1, 2, 3];
// (Chrome's XInput triggers count as pressed past 30 of 255.)
const TRIGGER_PRESSED = 30;

const open = [];                 // { dev, pad, alive }
let listening = false;

const isMac = () => /^mac/i.test((navigator.userAgentData && navigator.userAgentData.platform) || navigator.platform || "");
// Where the scan is offered: a Mac whose browser has WebUSB (Chrome, Edge).
export const usbScanOffered = () => isMac() && !!navigator.usb;
// The pads open now, as Gamepads.
export const usbPads = () => open.filter((e) => e.alive).map((e) => e.pad);

const isXInput = (alt) => alt.interfaceClass === XINPUT.classCode && alt.interfaceSubclass === XINPUT.subclassCode &&
                          alt.interfaceProtocol === XINPUT.protocolCode;
const hex4 = (n) => n.toString(16).padStart(4, "0");
const nameOf = (dev) => [dev.manufacturerName, dev.productName].filter(Boolean).join(" ").trim() || "Xbox 360 controller";
const short = (e) => String((e && e.message) || e).replace(/^Failed to execute '\w+' on '\w+': /, "");

function newPad(dev, index) {
  return {
    id: `${nameOf(dev)} (USB Vendor: ${hex4(dev.vendorId)} Product: ${hex4(dev.productId)})`,
    index, connected: true, mapping: "standard", timestamp: performance.now(),
    axes: [0, 0, 0, 0],
    buttons: Array.from({ length: 17 }, () => ({ pressed: false, touched: false, value: 0 })),
  };
}

// One report into the pad.  Anything that is not an input report is left alone.
export function readReport(pad, d) {
  if (d.byteLength < 14 || d.getUint8(0) !== 0x00) return false;
  const set = (i, value, pressed) => { const b = pad.buttons[i]; b.value = value; b.pressed = pressed; b.touched = value > 0; };
  const lo = d.getUint8(2), hi = d.getUint8(3);
  for (let bit = 0; bit < 8; bit++) {
    const on = (lo >> bit) & 1;
    set(LOW_BITS[bit], on, !!on);
    if (HIGH_BITS[bit] >= 0) { const h = (hi >> bit) & 1; set(HIGH_BITS[bit], h, !!h); }
  }
  for (const [i, at] of [[6, 4], [7, 5]]) { const t = d.getUint8(at); set(i, t / 255, t > TRIGGER_PRESSED); }
  const stick = (at) => { const v = d.getInt16(at, true); return v < 0 ? v / 32768 : v / 32767; };
  // (The standard layout's y is down positive.)
  pad.axes[0] = stick(6); pad.axes[1] = -stick(8); pad.axes[2] = stick(10); pad.axes[3] = -stick(12);
  pad.timestamp = performance.now();
  return true;
}

function drop(entry) {
  if (!entry.alive) return;
  entry.alive = false;
  entry.pad.connected = false;
  open.splice(open.indexOf(entry), 1);
  entry.dev.close().catch(() => {});
}

async function readLoop(entry, endpoint) {
  let bad = 0;
  while (entry.alive) {
    let r;
    try { r = await entry.dev.transferIn(endpoint, 32); }
    catch (e) { break; }                                  // unplugged, or closed
    if (r.status === "ok" && r.data) { bad = 0; readReport(entry.pad, r.data); continue; }
    if (++bad > 20) break;                                // (a pad that only stalls or babbles is given up)
    if (r.status === "stall") { try { await entry.dev.clearHalt("in", endpoint); } catch (e) { break; } }
  }
  drop(entry);
}

// Opens one pad and starts reading it: its name, or throws why not.
async function start(dev) {
  if (open.length >= MAX_PADS) throw new Error(`no more than ${MAX_PADS} at once`);
  const configs = dev.configuration ? [dev.configuration] : dev.configurations;
  if (!configs.some((c) => c.interfaces.some((i) => i.alternates.some(isXInput)))) throw new Error("not an Xbox 360-style pad");
  try {
    if (!dev.opened) await dev.open();
    if (!dev.configuration) await dev.selectConfiguration(configs.find((c) => c.interfaces.some((i) => i.alternates.some(isXInput))).configurationValue);
    const iface = dev.configuration.interfaces.find((i) => i.alternates.some(isXInput));
    const alt = iface.alternates.find(isXInput);
    try { await dev.claimInterface(iface.interfaceNumber); }
    catch (e) { throw new Error("it is in use (in another window or tab?)"); }
    if (alt.alternateSetting !== 0) await dev.selectAlternateInterface(iface.interfaceNumber, alt.alternateSetting);
    const input = alt.endpoints.find((e) => e.direction === "in" && e.type === "interrupt");
    const output = alt.endpoints.find((e) => e.direction === "out" && e.type === "interrupt");
    if (!input) throw new Error("it has no input endpoint");
    const used = new Set(open.map((e) => e.pad.index));
    let slot = 0;
    while (used.has(FIRST_INDEX + slot)) slot++;
    const entry = { dev, pad: newPad(dev, FIRST_INDEX + slot), alive: true };
    open.push(entry);
    // Its player light, where it has one (1 to 4, steady): 0x01 0x03 then 6 + the slot.
    if (output) dev.transferOut(output.endpointNumber, new Uint8Array([0x01, 0x03, 0x06 + slot])).catch(() => {});
    readLoop(entry, input.endpointNumber);
    return entry.pad.id;
  } catch (e) {
    if (dev.opened) dev.close().catch(() => {});
    throw e;
  }
}

// The scan: { added: [Gamepad ids], message } (the message a line for the page).
export async function scanUsbPads() {
  if (!usbScanOffered()) return { added: [], message: "this browser cannot read USB devices" };
  if (!listening) {
    listening = true;
    navigator.usb.addEventListener("disconnect", (ev) => { const e = open.find((x) => x.dev === ev.device); if (e) drop(e); });
  }
  const added = [], failed = [];
  const isOpen = (dev) => open.some((e) => e.dev === dev);
  let known = [];
  try { known = await navigator.usb.getDevices(); } catch (e) { /* none then */ }
  for (const dev of known) {
    if (isOpen(dev)) continue;
    try { added.push(await start(dev)); } catch (e) { failed.push(`${nameOf(dev)}: ${short(e)}`); }
  }
  if (!added.length) {
    let dev = null;
    try { dev = await navigator.usb.requestDevice({ filters: FILTERS }); }
    catch (e) {
      if (e && e.name === "NotFoundError") return { added, message: failed.length ? `Could not open ${failed.join("; ")}` : "No controller picked" };
      return { added, message: `The scan failed: ${short(e)}` };
    }
    if (isOpen(dev)) return { added, message: `${nameOf(dev)} is already in the list` };
    try { added.push(await start(dev)); } catch (e) { failed.push(`${nameOf(dev)}: ${short(e)}`); }
  }
  const found = added.length ? `Found ${added.map((id) => id.replace(/ \(USB .*$/, "")).join(", ")}` : "";
  const message = [found, failed.length ? `could not open ${failed.join("; ")}` : ""].filter(Boolean).join("; ");
  return { added, message: message.charAt(0).toUpperCase() + message.slice(1) };
}
