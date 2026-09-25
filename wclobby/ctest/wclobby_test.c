/*
 *  End-to-end test of the wclobby C API: a host and a client in one process
 *  join the same room, the client "connects" (first message), both exchange
 *  ordered messages including a large one, then hang up and reconnect over
 *  the same WebRTC link, and finally the client leaves.
 *
 *  Usage: wclobby_test [server] [code] [origin]
 *    server  default http://127.0.0.1:8789 (run the lobby with --allow-no-origin)
 *    origin  default "" (no Origin header); pass e.g. https://pqrstuvw.xyz for prod
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
    const char *server = argc > 1 ? argv[1] : "http://127.0.0.1:8789";
    const char *code = argc > 2 ? argv[2] : "WCTEST";
    const char *origin = argc > 3 ? argv[3] : "";
    char err[256];
    wclobby_buf_t buf;
    uint16_t p = 0;
    uint32_t hgen = 0, cgen = 0;
    int r, i;

    wclobby_set_logger(logger);
    wclobby_options_t o;
    memset(&o, 0, sizeof(o));
    o.server = server;
    o.code = code;
    o.origin = origin;
    o.create_max_players = 3;

    step("host joins (creates the room)");
    wclobby_t *host = wclobby_connect(&o, err, sizeof(err));
    CHECK(host, "host connect: %s", err);
    CHECK(wclobby_self_id(host) == 0, "host got slot %d", wclobby_self_id(host));
    CHECK(wclobby_max_players(host) == 3, "max players %d", wclobby_max_players(host));

    step("client joins");
    wclobby_t *client = wclobby_connect(&o, err, sizeof(err));
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

    step("client leaves the room; host sees the slot free up");
    wclobby_close(client);
    for (i = 0; i < 100 && wclobby_peer_state(host, 1, NULL) != WCLOBBY_PEER_ABSENT; i++) {
        usleep(100000);
    }
    CHECK(wclobby_peer_state(host, 1, NULL) == WCLOBBY_PEER_ABSENT, "client slot still %d",
          wclobby_peer_state(host, 1, NULL));
    CHECK(wclobby_open(host, 1, 0, &hgen) == -1, "open to an empty slot should fail at once");

    step("host leaves");
    wclobby_close(host);
    printf("OK\n");
    return 0;
}
