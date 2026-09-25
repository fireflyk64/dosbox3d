/*
 *  lobbylink transport: room codes instead of IP addresses.
 *
 *  Every player joins the same room on a lobbylink signaling server
 *  (lobbylink/README.md) and gets a WebRTC DataChannel to every other
 *  player, through NATs and firewalls (STUN, with a TURN relay as the
 *  fallback).  Player 0 -- the first one into the room -- runs the Wing
 *  Commander server; the others are clients and only talk to player 0.
 *  The payload is the same protobuf NetworkMessage as over TCP, one
 *  reliable DataChannel message per blob.
 *
 *  The WebRTC stack is the Rust crate in wclobby/ (a C ABI over
 *  lobbylink's Rust client) running on its own thread; LobbyStream and
 *  LobbyListener block on it the way the TCP transport blocks on sockets.
 */
#ifndef WCNET_LOBBY_H_
#define WCNET_LOBBY_H_

#include "dosbox.h"

#ifdef C_LOBBYLINK

#include <string>
#include "wcnet_transport.h"

struct wclobby;
class NetConfig;

namespace wc {

class LobbyHub {
public:
    // Joins (creating it if needed) the room in `cfg`.  Blocks until joined;
    // NULL on failure (logged).
    static LobbyHub *join(const NetConfig &cfg);
    ~LobbyHub();

    int self_id() const { return selfId_; }
    int max_players() const { return maxPlayers_; }
    bool is_host() const { return selfId_ == 0; }
    const std::string &code() const { return code_; }

    // Stream to `player` once its data channel is up (caller owns it).
    // NULL after timeoutMs, or right away when nobody holds that slot.
    Stream *open_peer(int player, int timeoutMs);
    // Hands out a stream for each peer that starts talking to us (caller
    // owns the listener).
    Listener *make_listener();

private:
    LobbyHub(wclobby *h, const std::string &code);
    wclobby *h_;
    std::string code_;
    int selfId_;
    int maxPlayers_;
};

// The joined room, or NULL.  Owned by init_network / uninit_network.
extern LobbyHub *g_lobby;

}  // namespace wc

#endif  // C_LOBBYLINK
#endif
