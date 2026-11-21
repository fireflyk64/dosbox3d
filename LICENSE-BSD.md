# Licenses: DOSBox is GPL-2.0, the new work here is BSD 2-Clause

This repository is DOSBox with our additions: the multiplayer layer for the
DOS space games (Wing Commander I and II, the Secret Ops and Special
Missions, Privateer, TIE Fighter and its demo, Terminal Velocity, Raptor), the
lobby, the RVGX sprite card, the browser page,
the tools and tests around them, and RetroVega, our own DOS game.

- **DOSBox proper is GPL-2.0** (`COPYING`), as it always was: every file that
  was in the tree at commit `bb94d5c4` (Boris Gjenero, 2015-05-10, "Properly
  detect ar, so emar is used with Emscripten") and every later change to
  those files. A build of the emulator (the native binary, `dosbox.wasm`)
  links DOSBox and is GPL-2.0 as a whole.
- **Every file added after `bb94d5c4` is BSD 2-Clause** (the text below),
  except the few named under "Not BSD" below. These files are the work of
  Daniel Reiter Horn and Patrick Reiter Horn, its only authors since that
  commit. Someone writing their own DOS emulator may take any of them into
  it without taking the GPL with them.
- **`lobbylink`** (a git submodule, the lobby server and its clients) is
  BSD 2-Clause as well, under its own `lobbylink/LICENSE`.
- **RetroVega** (`retrovega/`) has its own `retrovega/LICENSE`: BSD
  2-Clause, except the spaceship pictures rendered from Vega Strike's ship
  models (GPL-2.0), which it names one by one.

RetroVega alone, with its history, can be copied out into a repository of
its own with `scripts/retrovega-export.py` (this repository stays the source
of truth).

The rule covers files added later too: a file new since `bb94d5c4` is BSD
2-Clause unless it says otherwise. `git diff --name-only --diff-filter=A
bb94d5c4 HEAD` lists them.

## Not BSD

- `src/hardware/wctcp.cpp` (the `WCNET` shell command): adapted from DOSBox's
  own `IPXNET` command in `src/hardware/ipx.cpp`, so it is GPL-2.0 like DOSBox.
- `web/demos/1rap12.zip`, `web/demos/3dtv12.zip`: the Raptor and Terminal
  Velocity shareware releases, byte for byte, their publishers' to give away.
- `web/so3/so3.enc`, `web/so3/so3.json`: the SO3 campaign package, built
  from the original game's data and the Hellcat art by Mario Brito (HCl).
- The games themselves are never in this repository; the page asks for your
  own copy.

## BSD 2-Clause License

Copyright (c) 2015-2026 Daniel Reiter Horn and Patrick Reiter Horn
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

## The BSD files outside retrovega/ (as of this writing)

```
LICENSE-BSD.md
docs/newcampaign.md
docs/privateer.md
docs/raptor.md
docs/so3.md
docs/terminalvelocity.md
docs/tiedemo.md
docs/tiefighter.md
docs/wc2-port.md
docs/wcnet-multiplayer.md
notes.txt
runwc.sh
scripts/build-web.sh
scripts/privnet.sh
scripts/privrun.sh
scripts/privtests/coopsum.py
scripts/privtests/defend.sh
scripts/privtests/escort.sh
scripts/privtests/jump.sh
scripts/privtests/load.sh
scripts/privtests/loadsum.py
scripts/privtests/mission.sh
scripts/privtests/missions.sh
scripts/privtests/ships.sh
scripts/rapnet-duo.sh
scripts/raprun.sh
scripts/rapsig.py
scripts/retrovega-export.py
scripts/rvnet-abi.sh
scripts/rvnet-check.py
scripts/rvnet-duo.sh
scripts/site-shortcuts.sh
scripts/tdmcampaign-duo.sh
scripts/tdmmissions.py
scripts/tdmnet-duo.sh
scripts/tdmnet-sync.sh
scripts/tdmnet-syncmp.py
scripts/tdmrun.sh
scripts/tf-pa-sound.py
scripts/tfnet-aim.sh
scripts/tfnet-duo.sh
scripts/tfnet-join.py
scripts/tfnet-sheet.py
scripts/tfnet-sum.py
scripts/tfnet-video.py
scripts/tiedemo-unpack.mjs
scripts/tvexe.py
scripts/tvnet-duo.sh
scripts/tvnet-video.py
scripts/tvprof.py
scripts/tvrun.sh
scripts/tvsig.py
scripts/tvsigcheck.py
scripts/usbpad-mock.js
scripts/wc2data.py
scripts/wc2scene.py
scripts/wcdis.py
scripts/wcexe.py
scripts/wcmap.py
scripts/wcnet-smoke.sh
scripts/wcnet-wave-types.sh
scripts/wcport.py
scripts/web-build-skew.mjs
scripts/web-chatfilter-test.mjs
scripts/web-demos.mjs
scripts/web-gather.mjs
scripts/web-hall.mjs
scripts/web-legacy.mjs
scripts/web-pad.mjs
scripts/web-phone-steps.mjs
scripts/web-pick.mjs
scripts/web-pick.sh
scripts/web-priv.mjs
scripts/web-priv.sh
scripts/web-rap.mjs
scripts/web-rap.sh
scripts/web-room-keep.mjs
scripts/web-rv-difficulty.mjs
scripts/web-rv-drop.mjs
scripts/web-rv-flight.mjs
scripts/web-rv-phone.mjs
scripts/web-rv-saveload.mjs
scripts/web-rv-trio.mjs
scripts/web-rv.mjs
scripts/web-sim-pad.mjs
scripts/web-smoke.mjs
scripts/web-smoke.sh
scripts/web-tdm-flight.mjs
scripts/web-tdm-touch.mjs
scripts/web-tdm.mjs
scripts/web-tdm.sh
scripts/web-tf-guest.mjs
scripts/web-tf.sh
scripts/web-tiedemo.mjs
scripts/web-touch-test.mjs
scripts/web-trio.mjs
scripts/web-tv.mjs
scripts/web-tv.sh
scripts/web-usbpad.mjs
scripts/web-voice-test.mjs
scripts/web-voice.mjs
scripts/web-wc2-intro.mjs
scripts/web-wc2.mjs
scripts/web-wc2.sh
so3/README.md
so3/build.py
so3/campaign.py
so3/doorplates.py
so3/pack.mjs
so3/rig/README.md
so3/rig/campaignrun.sh
so3/rig/facecheck.sh
so3/rig/spawntest.sh
so3/rig/sweep.sh
so3/rig/w2duo.sh
so3/rig/w2solo.sh
so3/rig/w2trio.sh
so3/story.md
so3/texts.py
src/asyncify-remove.json
src/cpu/net_config.h
src/cpu/wc_net.h
src/cpu/wcnet_code.h
src/cpu/wcnet_ds.def
src/cpu/wcnet_entities.cpp
src/cpu/wcnet_entities.h
src/cpu/wcnet_events.cpp
src/cpu/wcnet_events.h
src/cpu/wcnet_game.cpp
src/cpu/wcnet_game.h
src/cpu/wcnet_hooks.cpp
src/cpu/wcnet_lab.cpp
src/cpu/wcnet_lab.h
src/cpu/wcnet_lobby.cpp
src/cpu/wcnet_lobby.h
src/cpu/wcnet_log.cpp
src/cpu/wcnet_log.h
src/cpu/wcnet_memory.cpp
src/cpu/wcnet_memory.h
src/cpu/wcnet_perf.cpp
src/cpu/wcnet_perf.h
src/cpu/wcnet_ports.h
src/cpu/wcnet_priv.cpp
src/cpu/wcnet_priv.h
src/cpu/wcnet_priv_controls.cpp
src/cpu/wcnet_priv_session.cpp
src/cpu/wcnet_rap.cpp
src/cpu/wcnet_rap.h
src/cpu/wcnet_rap_session.cpp
src/cpu/wcnet_rap_sig.h
src/cpu/wcnet_retrovega.cpp
src/cpu/wcnet_retrovega.h
src/cpu/wcnet_rv.h
src/cpu/wcnet_session.cpp
src/cpu/wcnet_session.h
src/cpu/wcnet_tie.cpp
src/cpu/wcnet_tie.h
src/cpu/wcnet_tie_cockpit.cpp
src/cpu/wcnet_tie_controls.cpp
src/cpu/wcnet_tie_ident.h
src/cpu/wcnet_tie_pa.cpp
src/cpu/wcnet_tie_pa.h
src/cpu/wcnet_tie_session.cpp
src/cpu/wcnet_tie_session.h
src/cpu/wcnet_tie_voice.cpp
src/cpu/wcnet_tiedemo.cpp
src/cpu/wcnet_tiedemo.h
src/cpu/wcnet_tiedemo_campaign.cpp
src/cpu/wcnet_tiedemo_controls.cpp
src/cpu/wcnet_tiedemo_missions.h
src/cpu/wcnet_tiedemo_session.cpp
src/cpu/wcnet_transport.cpp
src/cpu/wcnet_transport.h
src/cpu/wcnet_tv.cpp
src/cpu/wcnet_tv.h
src/cpu/wcnet_tv_pilot.cpp
src/cpu/wcnet_tv_session.cpp
src/cpu/wcnet_tv_sig.h
src/cpu/wcnet_tv_span.cpp
src/cpu/wcnet_vm.cpp
src/cpu/wcnet_vm.h
src/hardware/rvgx.cpp
src/hardware/rvgx_kernel.h
src/wc.proto
src/wclobby_web.js
src/web_runtime.js
tools/wc2wing/Cargo.lock
tools/wc2wing/Cargo.toml
tools/wc2wing/README.md
tools/wc2wing/src/main.rs
wclobby/.gitignore
wclobby/Cargo.lock
wclobby/Cargo.toml
wclobby/README.md
wclobby/ctest/run.sh
wclobby/ctest/wclobby_test.c
wclobby/include/wclobby.h
wclobby/src/lib.rs
web/build.js
web/chatfilter.js
web/gamefiles.js
web/gamepad.js
web/hall.js
web/index.html
web/inno-worker.js
web/pvm.js
web/serve.py
web/so3.js
web/usbpad.js
web/voice.js
web/wc.js
```
