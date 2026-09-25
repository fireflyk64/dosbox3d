/*
 *  wclobby: C ABI over the lobbylink Rust client (lobbylink/clients/rust)
 *  for DOSBox's Wing Commander multiplayer transport.
 *
 *  Model
 *  -----
 *  Joining a room gives every player a slot id 0..max_players-1 and a
 *  WebRTC reliable DataChannel to every other player.  On top of each
 *  physical link this layer keeps one *logical connection* at a time,
 *  identified by a generation number:
 *
 *    - the generation changes whenever the link comes up, dies, or either
 *      side hangs up (wclobby_hangup / a received hangup);
 *    - a caller holds a generation and passes it to send/recv; a mismatch
 *      means "your connection is gone", like EOF on a socket;
 *    - a peer becomes acceptable (wclobby_accept) when its first message
 *      arrives on a generation nobody has attached to yet, so hosts see
 *      "incoming connections" the way a TCP listener does, and a client
 *      can reconnect over the same link after a hangup.
 *
 *  Everything blocks on a background thread that owns the WebRTC stack;
 *  timeouts are in milliseconds, <0 = wait forever, 0 = poll.
 */
#ifndef WCLOBBY_H_
#define WCLOBBY_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wclobby wclobby_t;

typedef struct wclobby_options {
    const char *server;          /* "https://host[:port][/path]" of the lobby server; required */
    const char *code;            /* room code, 4-64 chars of [A-Za-z0-9_-]; required */
    const char *origin;          /* Origin header; NULL = derive from server, "" = send none */
    uint16_t create_max_players; /* >0: create the room with this many slots if it does not exist */
    int force_relay;             /* non-zero: TURN relay only (for testing) */
    const char *token_path;      /* file for resume-token persistence; NULL = none */
} wclobby_options_t;

/* Levels follow wcnet_log.h: 0 errors, 1 lifecycle, 2 per-event, 3 chatter.
 * Without a logger, messages go to stderr. */
typedef void (*wclobby_log_fn)(int level, const char *msg);
void wclobby_set_logger(wclobby_log_fn fn);

/* Joins (or creates) the room.  Blocks until joined or failed.  Returns NULL
 * on failure and, if err is non-NULL, writes "<code>: <message>" into it. */
wclobby_t *wclobby_connect(const wclobby_options_t *opts, char *err, size_t err_len);
/* Leaves the room and frees the handle.  Pending sends are flushed first. */
void wclobby_close(wclobby_t *h);

uint16_t wclobby_self_id(const wclobby_t *h);
uint16_t wclobby_max_players(const wclobby_t *h);

enum {
    WCLOBBY_PEER_ABSENT = 0, /* slot unoccupied */
    WCLOBBY_PEER_DOWN = 1,   /* occupied; data channel not (yet) open */
    WCLOBBY_PEER_UP = 2,     /* data channel open */
    WCLOBBY_PEER_GONE = 3    /* link failed or closed; a rebuilt link shows up as DOWN then UP */
};
/* Physical link state of a slot.  gen_out (optional) receives the current generation. */
int wclobby_peer_state(const wclobby_t *h, uint16_t player, uint32_t *gen_out);

/* Client side: waits until the link to `player` is up, attaches to it and
 * returns its generation.  1 = attached, 0 = timeout, -1 = closed, or the
 * slot is unoccupied. */
int wclobby_open(wclobby_t *h, uint16_t player, int32_t timeout_ms, uint32_t *gen_out);

/* Host side: waits for a peer with unread data on a generation nobody has
 * attached to, attaches to it and reports it.  1 = accepted, 0 = none
 * within the timeout, -1 = closed. */
int wclobby_accept(wclobby_t *h, int32_t timeout_ms, uint16_t *player_out, uint32_t *gen_out);

/* 1 while `gen` is the live logical connection to `player`, else 0. */
int wclobby_is_open(const wclobby_t *h, uint16_t player, uint32_t gen);

/* Queues one reliable, ordered message (up to 16 MiB) for the net thread.
 * 0 = queued, -1 = the connection is gone.  Delivery failures surface as a
 * generation change. */
int wclobby_send(wclobby_t *h, uint16_t to, uint32_t gen, const uint8_t *data, size_t len);

typedef struct wclobby_buf {
    uint8_t *data;
    size_t len;
} wclobby_buf_t;
/* Next message from `from` on connection `gen`.  1 = *out filled (release
 * with wclobby_buf_free), 0 = nothing within the timeout, -1 = the
 * connection is gone. */
int wclobby_recv(wclobby_t *h, uint16_t from, uint32_t gen, int32_t timeout_ms, wclobby_buf_t *out);
void wclobby_buf_free(wclobby_buf_t *buf);

/* Ends the logical connection `gen` to `player`: tells the peer (its
 * recv/send start failing) and bumps the generation locally.  The physical
 * link stays up for a later wclobby_open/wclobby_accept. */
void wclobby_hangup(wclobby_t *h, uint16_t player, uint32_t gen);

#ifdef __cplusplus
}
#endif

#endif
