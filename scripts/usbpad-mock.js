// A Mac with an Xbox 360-style pad on USB, for the page's tests (scripts/web-usbpad.mjs, and
// scripts/web-pad.mjs with PAD=usb): put in before the page's own scripts (addInitScript({ path })).
// navigator.platform and userAgentData say macOS, and navigator.usb is a stand-in holding one
// Logitech F310 with its switch on X, whose reports are what a real one sends (web/usbpad.js):
// a 20-byte input report only when something changes, after an LED and a status message.
//
// window.__usb, for the test:
//   calls             every use the page made of navigator.usb ("get <name>"), in order
//   requests          the filters of each requestDevice
//   chooser           "pick" (the default) or "cancel": what the player does in the browser's chooser
//   claimFails        claimInterface throws (the pad open in another tab)
//   outs              what was sent to the pad (arrays of bytes)
//   button(i, v)      button i of the standard layout to v (0..1; 6 and 7 the triggers)
//   axis(i, v)        axis i of the standard layout to v (-1..1; 1 and 3 down positive)
//   unplug(), plug()  the cable
// The browser's permission (a device picked once) is kept in localStorage across reloads, as the
// browser keeps it.
(() => {
  const mac = { platform: "macOS", mobile: false, brands: [], getHighEntropyValues: async () => ({ platform: "macOS" }) };
  Object.defineProperty(Navigator.prototype, "platform", { get: () => "MacIntel", configurable: true });
  Object.defineProperty(Navigator.prototype, "userAgentData", { get: () => mac, configurable: true });

  const T = window.__usb = { calls: [], requests: [], chooser: "pick", claimFails: false, outs: [] };
  const GRANTED = "__usbmock:granted";
  const granted = () => { try { return localStorage.getItem(GRANTED) === "1"; } catch (e) { return false; } };
  const grant = () => { try { localStorage.setItem(GRANTED, "1"); } catch (e) { /* this page only */ } };

  const state = { lo: 0, hi: 0, lt: 0, rt: 0, lx: 0, ly: 0, ry: 0, rx: 0 };
  // Standard button -> [byte ("lo" 2 or "hi" 3), bit]; 6 and 7 are the triggers.
  const BITS = { 0: ["hi", 4], 1: ["hi", 5], 2: ["hi", 6], 3: ["hi", 7], 4: ["hi", 0], 5: ["hi", 1], 8: ["lo", 5], 9: ["lo", 4],
                 10: ["lo", 6], 11: ["lo", 7], 12: ["lo", 0], 13: ["lo", 1], 14: ["lo", 2], 15: ["lo", 3], 16: ["hi", 2] };
  const toInt = (v) => Math.max(-32768, Math.min(32767, v < 0 ? Math.round(v * 32768) : Math.round(v * 32767)));

  const queue = [], waiters = [];
  let plugged = true, isOpen = false;
  const send = (bytes) => {
    const data = new DataView(new Uint8Array(bytes).buffer);
    const w = waiters.shift();
    if (w) w.resolve({ status: "ok", data }); else queue.push(data);
  };
  const report = () => {
    const b = new Uint8Array(20); const d = new DataView(b.buffer);
    b[0] = 0x00; b[1] = 0x14; b[2] = state.lo; b[3] = state.hi; b[4] = state.lt; b[5] = state.rt;
    d.setInt16(6, state.lx, true); d.setInt16(8, state.ly, true); d.setInt16(10, state.rx, true); d.setInt16(12, state.ry, true);
    send(Array.from(b));
  };
  T.button = (i, v) => {
    if (i === 6 || i === 7) state[i === 6 ? "lt" : "rt"] = Math.round(v * 255);
    else { const [k, bit] = BITS[i]; state[k] = v > 0.5 ? state[k] | (1 << bit) : state[k] & ~(1 << bit); }
    report();
  };
  T.axis = (i, v) => {
    // (the standard layout's y is down positive, the pad's up positive)
    const [k, s] = [["lx", 1], ["ly", -1], ["rx", 1], ["ry", -1]][i];
    state[k] = toInt(s * v);
    report();
  };

  const endpoints = [{ endpointNumber: 1, direction: "in", type: "interrupt", packetSize: 32 },
                     { endpointNumber: 2, direction: "out", type: "interrupt", packetSize: 32 }];
  const alt = { alternateSetting: 0, interfaceClass: 0xff, interfaceSubclass: 0x5d, interfaceProtocol: 0x01, interfaceName: null, endpoints };
  const config = { configurationValue: 1, configurationName: null, interfaces: [{ interfaceNumber: 0, claimed: false, alternate: alt, alternates: [alt] }] };
  const fail = (what) => Promise.reject(new DOMException(`Failed to execute '${what}' on 'USBDevice': The device was disconnected.`, "NotFoundError"));
  const dev = {
    vendorId: 0x046d, productId: 0xc21d, manufacturerName: "Logitech", productName: "Gamepad F310", serialNumber: null,
    usbVersionMajor: 2, deviceClass: 0xff, deviceSubclass: 0xff, deviceProtocol: 0xff,
    configuration: null, configurations: [config],
    get opened() { return isOpen; },
    async open() {
      if (!plugged) return fail("open");
      isOpen = true; queue.length = 0;
      // (what a pad says first: its LEDs blinking, its link, then where everything is)
      send([0x01, 0x03, 0x0e]); send([0x02, 0x03, 0x00]); report();
    },
    async close() { isOpen = false; config.interfaces[0].claimed = false; for (const w of waiters.splice(0)) w.reject(new DOMException("The device was closed.", "AbortError")); },
    async selectConfiguration(n) { if (!isOpen) return fail("selectConfiguration"); this.configuration = config; },
    async claimInterface(n) {
      if (!isOpen) return fail("claimInterface");
      if (T.claimFails) throw new DOMException("Failed to execute 'claimInterface' on 'USBDevice': Unable to claim interface.", "NetworkError");
      config.interfaces[0].claimed = true;
    },
    async releaseInterface() { config.interfaces[0].claimed = false; },
    async selectAlternateInterface() {},
    async clearHalt() {},
    transferIn(ep, len) {
      if (!plugged || !isOpen) return fail("transferIn");
      if (queue.length) return Promise.resolve({ status: "ok", data: queue.shift() });
      return new Promise((resolve, reject) => waiters.push({ resolve, reject }));
    },
    async transferOut(ep, data) {
      if (!plugged || !isOpen) return fail("transferOut");
      T.outs.push(Array.from(new Uint8Array(data.buffer || data)));
      return { status: "ok", bytesWritten: data.byteLength };
    },
  };

  const target = new EventTarget();
  const usb = {
    getDevices: async () => (plugged && granted() ? [dev] : []),
    requestDevice: async (opts) => {
      T.requests.push(JSON.parse(JSON.stringify(opts.filters)));
      if (T.chooser === "cancel" || !plugged) throw new DOMException("No device selected.", "NotFoundError");
      grant();
      return dev;
    },
    addEventListener: (...a) => target.addEventListener(...a),
    removeEventListener: (...a) => target.removeEventListener(...a),
    onconnect: null, ondisconnect: null,
  };
  T.unplug = () => {
    plugged = false; isOpen = false; dev.configuration = null;
    for (const w of waiters.splice(0)) w.reject(new DOMException("A transfer error has occurred.", "NetworkError"));
    const ev = new Event("disconnect"); ev.device = dev; target.dispatchEvent(ev);
  };
  T.plug = () => { plugged = true; const ev = new Event("connect"); ev.device = dev; target.dispatchEvent(ev); };
  const watched = new Proxy(usb, { get(t, k) { T.calls.push("get " + String(k)); return t[k]; } });
  Object.defineProperty(Navigator.prototype, "usb", { get: () => watched, configurable: true });
})();
