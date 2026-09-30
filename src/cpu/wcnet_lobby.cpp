#include "dosbox.h"

#ifdef C_LOBBYLINK

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wclobby.h"
#include "wcnet_lobby.h"
#include "wcnet_log.h"
#include "net_config.h"

#ifdef EMSCRIPTEN
// In the browser the C API is implemented by src/wclobby_web.js, which
// reads wclobby_options_t and wclobby_buf_t at these wasm32 offsets.
#include <stddef.h>
static_assert(offsetof(wclobby_options_t, server) == 0, "wclobby_web.js layout");
static_assert(offsetof(wclobby_options_t, code) == 4, "wclobby_web.js layout");
static_assert(offsetof(wclobby_options_t, origin) == 8, "wclobby_web.js layout");
static_assert(offsetof(wclobby_options_t, create_max_players) == 12, "wclobby_web.js layout");
static_assert(offsetof(wclobby_options_t, force_relay) == 16, "wclobby_web.js layout");
static_assert(offsetof(wclobby_options_t, token_path) == 20, "wclobby_web.js layout");
static_assert(offsetof(wclobby_options_t, no_keepalive) == 24, "wclobby_web.js layout");
static_assert(offsetof(wclobby_buf_t, len) == 4 && sizeof(wclobby_buf_t) == 8, "wclobby_web.js layout");
#endif

namespace wc {

LobbyHub *g_lobby = NULL;

static void lobby_log(int level, const char *msg) {
    wclog(level, "lobby: %s", msg);
}

// ---------------------------------------------------------------------------
// LobbyStream: one logical connection (generation) to one player

class LobbyStream : public Stream {
public:
    LobbyStream(wclobby *h, int player, uint32_t gen)
        : h_(h), player_((uint16_t)player), gen_(gen), closed_(false) {
        char buf[48];
        snprintf(buf, sizeof(buf), "lobby player %d", player);
        name_ = buf;
    }
    ~LobbyStream() { close(); }

    virtual bool is_open() const {
        return !closed_ && wclobby_is_open(h_, player_, gen_) == 1;
    }

    virtual void close() {
        if (!closed_) {
            closed_ = true;
            wclobby_hangup(h_, player_, gen_);
        }
    }

    virtual bool send(const std::string &bytes) {
        if (closed_) {
            return false;
        }
        if (wclobby_send(h_, player_, gen_, (const uint8_t *)bytes.data(), bytes.size()) != 0) {
            wclog(1, "%s: connection is gone", name_.c_str());
            return false;
        }
        return true;
    }

    virtual RecvStatus recv(std::string &bytes, bool blocking) {
        if (closed_) {
            return RecvStatus::STATUS_FAIL;
        }
        wclobby_buf_t buf = { NULL, 0 };
        int r = wclobby_recv(h_, player_, gen_, blocking ? -1 : 0, &buf);
        if (r == 1) {
            bytes.assign((const char *)buf.data, buf.len);
            wclobby_buf_free(&buf);
            return RecvStatus::STATUS_OK;
        }
        if (r == 0) {
            return RecvStatus::STATUS_NO_DATA;
        }
        wclog(1, "%s: connection closed", name_.c_str());
        return RecvStatus::STATUS_FAIL;
    }

    virtual std::string describe() const { return name_; }

private:
    wclobby *h_;
    uint16_t player_;
    uint32_t gen_;
    bool closed_;
    std::string name_;
};

// ---------------------------------------------------------------------------
// LobbyListener: peers become connections when they start talking

class LobbyListener : public Listener {
public:
    LobbyListener(wclobby *h, const std::string &code) : h_(h), code_(code) {}

    virtual Stream *accept(bool blocking) {
        uint16_t player = 0;
        uint32_t gen = 0;
        int r = wclobby_accept(h_, blocking ? -1 : 0, &player, &gen);
        if (r == 1) {
            return new LobbyStream(h_, player, gen);
        }
        if (r < 0) {
            wclog(0, "lobby: room %s is closed", code_.c_str());
        }
        return NULL;
    }

    virtual std::string describe() const { return "room " + code_; }

private:
    wclobby *h_;
    std::string code_;
};

// ---------------------------------------------------------------------------
// LobbyHub

LobbyHub::LobbyHub(wclobby *h, const std::string &code)
    : h_(h), code_(code), selfId_(wclobby_self_id(h)), maxPlayers_(wclobby_max_players(h)) {}

LobbyHub::~LobbyHub() {
    wclog(1, "leaving room %s", code_.c_str());
    wclobby_close(h_);
}

LobbyHub *LobbyHub::join(const NetConfig &cfg) {
    wclobby_set_logger(lobby_log);
    wclobby_options_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.server = cfg.lobby_url;
    opts.code = cfg.room;
    opts.origin = cfg.lobby_origin;
    opts.create_max_players = (uint16_t)cfg.lobby_players;
    opts.force_relay = cfg.lobby_relay ? 1 : 0;
    opts.token_path = NULL;
    char err[256];
    err[0] = 0;
    wclobby *h = wclobby_connect(&opts, err, sizeof(err));
    if (!h) {
        wclog(0, "could not join room %s at %s: %s", cfg.room, cfg.lobby_url, err);
        return NULL;
    }
    return new LobbyHub(h, cfg.room);
}

Stream *LobbyHub::open_peer(int player, int timeoutMs) {
    uint32_t gen = 0;
    int state = wclobby_peer_state(h_, (uint16_t)player, NULL);
    if (state == WCLOBBY_PEER_ABSENT) {
        wclog(0, "nobody holds slot %d in room %s", player, code_.c_str());
        return NULL;
    }
    if (state != WCLOBBY_PEER_UP) {
        wclog(1, "waiting for a connection to player %d", player);
    }
    int r = wclobby_open(h_, (uint16_t)player, timeoutMs, &gen);
    if (r == 1) {
        return new LobbyStream(h_, player, gen);
    }
    if (r == 0) {
        wclog(0, "no connection to player %d after %d ms", player, timeoutMs);
    } else {
        wclog(0, "player %d is not reachable", player);
    }
    return NULL;
}

Listener *LobbyHub::make_listener() {
    return new LobbyListener(h_, code_);
}

bool LobbyHub::signaling_alive() const {
    return wclobby_signaling_alive(h_) == 1;
}

}  // namespace wc

#endif  // C_LOBBYLINK
