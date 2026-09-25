/*
 *  TCP transport for the Wing Commander multiplayer protocol.
 *
 *  Messages are length-prefixed (3 big-endian bytes) protobuf NetworkMessage
 *  blobs.  A Connection keeps one queue per message category so that a
 *  blocking read of a game frame can park chat messages (and vice versa)
 *  instead of losing them.
 */
#ifndef WCNET_TRANSPORT_H_
#define WCNET_TRANSPORT_H_

#include <deque>
#include <string>
#include "../wc.pb.h"

namespace wc {

class RecvStatus {
public:
    enum Type { OK, FAIL, NO_DATA };
    RecvStatus(Type t = FAIL) : t_(t) {}
    bool ok() const { return t_ == OK; }
    bool no_data() const { return t_ == NO_DATA; }
    bool failed() const { return t_ == FAIL; }

private:
    Type t_;
};

enum MessageCategory {
    CAT_GAME = 0,   // frames, handshakes, mission state
    CAT_CHAT = 1,   // chat; never blocks
    NUM_CATEGORIES
};

MessageCategory category_of(const NetworkMessage &msg);
const char *message_type_name(const NetworkMessage &msg);

class Connection {
public:
    Connection();
    ~Connection();

    void adopt(int fd);
    void close();
    bool is_open() const { return fd_ != -1; }

    bool send(const NetworkMessage &msg);

    // Blocking receive of the next message of `cat`.  Messages of other
    // categories that arrive meanwhile are queued.
    RecvStatus recv(MessageCategory cat, NetworkMessage &msg);
    // Non-blocking: returns NO_DATA when nothing of `cat` is available.  Reads
    // at most one message from the socket per call.
    RecvStatus poll(MessageCategory cat, NetworkMessage &msg);
    // Look at the next queued message of `cat` without consuming it.
    const NetworkMessage *peek(MessageCategory cat) const;
    size_t queued(MessageCategory cat) const { return queues_[cat].size(); }

private:
    RecvStatus read_one(NetworkMessage &msg, bool blocking);
    bool socket_readable() const;

    int fd_;
    std::deque<NetworkMessage> queues_[NUM_CATEGORIES];
};

// Returns a listening socket or -1.
int listen_on(const char *portstr);
// Accepts one pending connection; -1 when none (non-blocking) or on error.
int accept_connection(int listenFd, bool blocking);
// Connects to host:port; -1 on failure.
int connect_to(const char *host, const char *portstr);
void close_socket(int fd);

}  // namespace wc

#endif
