/*
 *  End-to-end test of the wclobby C API: a host and a client in one process
 *  join the same room, the client "connects" (first message), both exchange
 *  ordered messages including a large one, then hang up and reconnect over
 *  the same WebRTC link, and finally the client leaves.
 *
 *  Usage: wclobby_test [server] [code] [origin] [--soak SECONDS] [--no-keepalive]
 *    server  default http://127.0.0.1:8789 (run the lobby with --allow-no-origin)
 *    origin  default "" (no Origin header); pass e.g. https://pqrstuvw.xyz for prod
 *    --soak  after the exchange, sit idle that long, then check that the
 *            signaling connections and the peer link survived (what an
 *            idle proxy or NAT does to a long game)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "wclobby.h"

static void logger(int level, const char *msg) {
    fprintf(stderr, "  [lobby %d] %s\n", level, msg);
}

#define CHECK(cond, ...)                                        \
    do {                                                        \
        if (!(cond)) {                                          \
            fprintf(stderr, "FAIL at line %d: ", __LINE__);     \
            fprintf(stderr, __VA_ARGS__);                       \
            fprintf(stderr, "\n");                              \
            exit(1);                                            \
        }                                                       \
    } while (0)

static void step(const char *what) {
    printf("- %s\n", what);
    fflush(stdout);
}

int main(int argc, char **argv) {
    const char *positional[3] = { "http://127.0.0.1:8789", "WCTEST", "" };
    int npos = 0, soak = 0, no_keepalive = 0;
    char err[256];
    wclobby_buf_t buf;
    uint16_t p = 0;
    uint32_t hgen = 0, cgen = 0;
    int r, i;

    const char *client_server = NULL;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--soak") == 0 && i + 1 < argc) {
            soak = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--no-keepalive") == 0) {
            no_keepalive = 1;
        } else if (strcmp(argv[i], "--client-server") == 0 && i + 1 < argc) {
            /* The client joins through this URL instead (e.g. a relay that
             * can be killed to cut only its signaling connection). */
            client_server = argv[++i];
        } else if (npos < 3) {
            positional[npos++] = argv[i];
        }
    }
    const char *server = positional[0];
    const char *code = positional[1];
    const char *origin = positional[2];

    wclobby_set_logger(logger);
    wclobby_options_t o;
    memset(&o, 0, sizeof(o));
    o.server = server;
    o.code = code;
    o.origin = origin;
    o.create_max_players = 3;
    o.no_keepalive = no_keepalive;

    step("host joins (creates the room)");
    wclobby_t *host = wclobby_connect(&o, err, sizeof(err));
    CHECK(host, "host connect: %s", err);
    CHECK(wclobby_self_id(host) == 0, "host got slot %d", wclobby_self_id(host));
    CHECK(wclobby_max_players(host) == 3, "max players %d", wclobby_max_players(host));

    step("client joins");
    if (client_server) {
        o.server = client_server;
    }
    wclobby_t *client = wclobby_connect(&o, err, sizeof(err));
    o.server = server;
    CHECK(client, "client connect: %s", err);
    CHECK(wclobby_self_id(client) == 1, "client got slot %d", wclobby_self_id(client));

    step("client opens a connection to the host (waits for WebRTC)");
    r = wclobby_open(client, 0, 30000, &cgen);
    CHECK(r == 1, "open host: %d", r);
    CHECK(wclobby_is_open(client, 0, cgen) == 1, "client not open");
    CHECK(wclobby_accept(host, 0, &p, &hgen) == 0, "host accepted before any data");
    CHECK(wclobby_recv(client, 0, cgen, 0, &buf) == 0, "client poll on empty");

    step("client introduces itself; host accepts");
    CHECK(wclobby_send(client, 0, cgen, (const uint8_t *)"connect", 7) == 0, "send connect");
    r = wclobby_accept(host, 20000, &p, &hgen);
    CHECK(r == 1 && p == 1, "accept: r=%d player=%d", r, p);
    r = wclobby_recv(host, 1, hgen, 5000, &buf);
    CHECK(r == 1 && buf.len == 7 && memcmp(buf.data, "connect", 7) == 0, "recv connect: %d", r);
    wclobby_buf_free(&buf);
    CHECK(buf.data == NULL, "buf not cleared");

    step("host sends 500 frames in order");
    for (i = 0; i < 500; i++) {
        char m[64];
        int n = snprintf(m, sizeof(m), "frame %d", i);
        CHECK(wclobby_send(host, 1, hgen, (const uint8_t *)m, n) == 0, "send frame %d", i);
    }
    for (i = 0; i < 500; i++) {
        char m[64];
        int n = snprintf(m, sizeof(m), "frame %d", i);
        r = wclobby_recv(client, 0, cgen, 5000, &buf);
        CHECK(r == 1 && (int)buf.len == n && memcmp(buf.data, m, n) == 0, "frame %d: r=%d", i, r);
        wclobby_buf_free(&buf);
    }

    step("client sends a 300 KB message (chunked reliable)");
    {
        size_t big = 300000;
        uint8_t *bd = malloc(big);
        size_t k;
        for (k = 0; k < big; k++) {
            bd[k] = (uint8_t)(k * 7 + 3);
        }
        CHECK(wclobby_send(client, 0, cgen, bd, big) == 0, "big send");
        r = wclobby_recv(host, 1, hgen, 20000, &buf);
        CHECK(r == 1 && buf.len == big && memcmp(buf.data, bd, big) == 0, "big recv: r=%d len=%zu", r, buf.len);
        wclobby_buf_free(&buf);
        free(bd);
    }
    CHECK(wclobby_recv(host, 1, hgen, 0, &buf) == 0, "poll after big");

    if (soak > 0) {
        int elapsed;
        printf("- idling for %d s (keepalive %s)\n", soak, no_keepalive ? "off" : "on");
        for (elapsed = 0; elapsed < soak; elapsed += 30) {
            sleep(soak - elapsed < 30 ? soak - elapsed : 30);
            printf("  t=%ds signaling host=%d client=%d, link host=%d client=%d\n",
                   elapsed + 30 < soak ? elapsed + 30 : soak,
                   wclobby_signaling_alive(host), wclobby_signaling_alive(client),
                   wclobby_peer_state(host, 1, NULL), wclobby_peer_state(client, 0, NULL));
            fflush(stdout);
        }
        step("after the idle period: the game link still works both ways");
        CHECK(wclobby_is_open(host, 1, hgen) == 1, "host connection died during the idle period");
        CHECK(wclobby_is_open(client, 0, cgen) == 1, "client connection died during the idle period");
        CHECK(wclobby_send(host, 1, hgen, (const uint8_t *)"still", 5) == 0, "send after idle");
        r = wclobby_recv(client, 0, cgen, 10000, &buf);
        CHECK(r == 1 && buf.len == 5, "recv after idle: %d", r);
        wclobby_buf_free(&buf);
        CHECK(wclobby_send(client, 0, cgen, (const uint8_t *)"there", 5) == 0, "send back after idle");
        r = wclobby_recv(host, 1, hgen, 10000, &buf);
        CHECK(r == 1 && buf.len == 5, "recv back after idle: %d", r);
        wclobby_buf_free(&buf);
        printf("  signaling after idle: host=%d client=%d\n",
               wclobby_signaling_alive(host), wclobby_signaling_alive(client));
        if (!no_keepalive) {
            CHECK(wclobby_signaling_alive(host) && wclobby_signaling_alive(client),
                  "signaling dropped despite the keepalive");
        }
    }

    step("client hangs up; host sees EOF");
    wclobby_hangup(client, 0, cgen);
    CHECK(wclobby_is_open(client, 0, cgen) == 0, "client still open after own hangup");
    r = wclobby_recv(host, 1, hgen, 5000, &buf);
    CHECK(r == -1, "host recv after hangup: %d", r);
    CHECK(wclobby_is_open(host, 1, hgen) == 0, "host still open after hangup");
    CHECK(wclobby_send(host, 1, hgen, (const uint8_t *)"x", 1) == -1, "host send after hangup succeeded");
    CHECK(wclobby_peer_state(host, 1, NULL) == WCLOBBY_PEER_UP, "link should survive a hangup");

    step("client reconnects over the same link");
    r = wclobby_open(client, 0, 5000, &cgen);
    CHECK(r == 1, "reopen: %d", r);
    CHECK(wclobby_send(client, 0, cgen, (const uint8_t *)"again", 5) == 0, "send again");
    r = wclobby_accept(host, 5000, &p, &hgen);
    CHECK(r == 1 && p == 1, "re-accept: r=%d player=%d", r, p);
    r = wclobby_recv(host, 1, hgen, 5000, &buf);
    CHECK(r == 1 && buf.len == 5 && memcmp(buf.data, "again", 5) == 0, "recv again: %d", r);
    wclobby_buf_free(&buf);
    CHECK(wclobby_send(host, 1, hgen, (const uint8_t *)"welcome", 7) == 0, "send welcome");
    r = wclobby_recv(client, 0, cgen, 5000, &buf);
    CHECK(r == 1 && buf.len == 7, "recv welcome: %d", r);
    wclobby_buf_free(&buf);

    step("host hangs up; client sees EOF");
    wclobby_hangup(host, 1, hgen);
    r = wclobby_recv(client, 0, cgen, 5000, &buf);
    CHECK(r == -1, "client recv after host hangup: %d", r);

    if (client_server || !wclobby_signaling_alive(host) || !wclobby_signaling_alive(client)) {
        /* Without signaling on both sides the client's leave never reaches
         * the lobby, or the host never hears about it: the slot stays
         * occupied until the lobby's claim timeout and the host only learns
         * of the exit from the peer link going down (ICE consent timeout). */
        step("client leaves (signaling is gone on one side; the slot stays occupied)");
        wclobby_close(client);
    } else {
        step("client leaves the room; host sees the slot free up");
        wclobby_close(client);
        for (i = 0; i < 100 && wclobby_peer_state(host, 1, NULL) != WCLOBBY_PEER_ABSENT; i++) {
            usleep(100000);
        }
        CHECK(wclobby_peer_state(host, 1, NULL) == WCLOBBY_PEER_ABSENT, "client slot still %d",
              wclobby_peer_state(host, 1, NULL));
        CHECK(wclobby_open(host, 1, 0, &hgen) == -1, "open to an empty slot should fail at once");
    }

    step("host leaves");
    wclobby_close(host);
    printf("OK\n");
    return 0;
}
