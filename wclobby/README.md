# wclobby — lobbylink transport for Wing Commander multiplayer

Friends rarely have a reachable IP address any more, so besides the TCP
transport (`WCHOST` / `WCPORT`) DOSBox can meet the other players in a
[lobbylink](../lobbylink/README.md) room: everyone joins the same **room
code** on a lobby server and gets a WebRTC DataChannel to everyone else,
through NAT and firewalls (STUN first, TURN relay as the fallback).

```
./runwc.sh /path/to/wc room FALCON-7      # everyone runs this with the same code
```

The first player into the room hosts the game (slot 0 = Wing Commander
server); the rest join it as clients. Inside DOSBox the equivalent is
`WCNET ROOM FALCON-7 [server-url]`, and `WCNET STATUS` shows the role.

Environment: `WCROOM` (room code), `WCLOBBY` (lobby server URL, default
`https://pqrstuvw.xyz/lobbylink`), `WCPLAYERS` (room size, default 3),
`WCLOBBY_ORIGIN` (Origin header; `""` = none, for a local server started
with `--allow-no-origin`), `WCLOBBY_RELAY=1` (force the TURN relay),
`WCNET_LOG=0..3` (verbosity).

## How it is put together

```
src/cpu/wcnet_session.cpp   game protocol; unchanged, talks to wc::Connection
src/cpu/wcnet_transport.*   wc::Stream / wc::Listener abstraction + the TCP one
src/cpu/wcnet_lobby.*       LobbyStream / LobbyListener / LobbyHub over the C API
wclobby/include/wclobby.h   the C API
wclobby/src/lib.rs          Rust: tokio thread owning lobbylink's P2PGame (native)
src/wclobby_web.js          JavaScript: the same C API for the Emscripten build,
                            over lobbylink's browser client (Asyncify suspends
                            the wasm instead of blocking a thread)
lobbylink/clients/rust      the lobbylink client (WebSocket signaling + webrtc)
lobbylink/clients/ts        the browser client used by the web build
```

The browser build (`scripts/build-web.sh`, see the top-level README) does
not use cargo at all: `configure` picks `src/wclobby_web.js` when compiling
with Emscripten.  Both implementations keep the same generation / hangup
semantics, so a browser player and a native player can share a room.

`lobbylink/` is a git submodule (`git submodule update --init` after a
fresh clone); this crate depends on its Rust client by path.
`cargo build --release` in this directory produces
`target/release/libwclobby.a`; `configure` enables it automatically when
`cargo` is on the PATH and the submodule is checked out
(`--disable-lobbylink` turns it off), and `src/Makefile.am` runs cargo as
part of `make`.

The Rust side turns lobbylink's single event stream into per-player
inboxes with blocking receives. On top of each physical WebRTC link it
keeps one *logical connection* at a time, numbered by a generation that
changes when the link comes up or dies and when either side hangs up
(a zero-length message, which the game protocol never sends). A host
"accepts" a player when that player's first message arrives on a fresh
generation, which is how the game's connect/accept flow — and reconnects
after a dropped session — map onto a link that stays up.

Once every peer link is up a game sends nothing over the signaling
WebSocket, and idle proxies and NATs drop such connections after a few
minutes. The Rust client therefore pings the lobby server every 25 s
(`disable_keepalive` / `no_keepalive` turns that off). If the signaling
connection is lost anyway, established peer links keep working, the
lobby's "player disconnected" notice about the other side is ignored,
and only the physical link failing, an explicit leave or a hangup ends a
game connection; `WCNET STATUS` then notes that nobody new can join.

## Testing

`ctest/run.sh` builds a local lobby server from `../lobbylink` (needs Go)
and runs `ctest/wclobby_test.c` against it: join, accept, 500 ordered
messages, a 300 KB message, hangup, reconnect over the same link, leave.
`ctest/run.sh https://pqrstuvw.xyz/lobbylink` runs the same test through
the public server. `wclobby_test ... --soak 480 [--no-keepalive]` idles
that long after the exchange and then checks that the link (and, with
the keepalive, the signaling connection) survived; `--client-server URL`
joins the client through another URL, e.g. a `socat` relay you can kill
to cut only its signaling connection mid-session.
