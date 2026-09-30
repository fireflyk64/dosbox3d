/*
 *  web_runtime.js: small runtime tweaks for the Emscripten build.
 *
 *  emscripten_sleep(0) is how the emulator (and SDL's framebuffer update)
 *  yields to the browser, and it happens dozens of times per second.
 *  Emscripten implements it with setTimeout, which Chrome clamps to 4 ms
 *  once timers are nested (they always are here: each wake-up starts the
 *  next sleep), so the emulator would spend most of its time waiting.  A
 *  0 ms sleep is therefore done with a MessageChannel task, which still
 *  returns to the event loop (input events, WebRTC messages, painting)
 *  but resumes right away.  Longer sleeps keep using a timer.
 */
addToLibrary({
  $WEB_RUNTIME: {
    channel: null,
    pending: [],
    yieldNow(fn) {
      if (!WEB_RUNTIME.channel) {
        WEB_RUNTIME.channel = new MessageChannel();
        WEB_RUNTIME.channel.port1.onmessage = () => {
          var fns = WEB_RUNTIME.pending;
          WEB_RUNTIME.pending = [];
          fns.forEach((f) => f());
        };
      }
      WEB_RUNTIME.pending.push(fn);
      WEB_RUNTIME.channel.port2.postMessage(0);
    },
  },
  emscripten_sleep__deps: ['$WEB_RUNTIME'],
  emscripten_sleep__async: true,
  emscripten_sleep: (ms) => Asyncify.handleSleep((wakeUp) => {
    if (ms > 0) setTimeout(wakeUp, ms);
    else WEB_RUNTIME.yieldNow(wakeUp);
  }),
});
