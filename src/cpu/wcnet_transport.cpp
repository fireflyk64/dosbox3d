#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <vector>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include "wcnet_transport.h"
#include "wcnet_log.h"

namespace wc {

MessageCategory category_of(const NetworkMessage &msg) {
    return msg.has_chat() ? CAT_CHAT : CAT_GAME;
}

const char *message_type_name(const NetworkMessage &msg) {
    if (msg.has_connect()) return "connect";
    if (msg.has_game()) return "game";
    if (msg.has_frame()) return "frame";
    if (msg.has_chat()) return "chat";
    if (msg.has_start_mission_req()) return "start_mission_req";
    if (msg.has_start_briefing_req()) return "start_briefing_req";
    if (msg.has_briefing_start()) return "briefing_start";
    return "none";
}

template <class Fn>
static ssize_t xfer_all(const Fn &fn, unsigned char *buf, size_t size) {
    size_t done = 0;
    while (size) {
        ssize_t cur = fn(buf, size);
        if (cur < 0) {
            if (errno == EINTR) {
                continue;
            }
            return cur;
        }
        if (cur == 0) {
            return (ssize_t)done;
        }
        done += cur;
        buf += cur;
        size -= cur;
    }
    return (ssize_t)done;
}

struct SendFn {
    int fd;
    ssize_t operator()(unsigned char *b, size_t n) const { return ::send(fd, b, n, 0); }
};
struct RecvFn {
    int fd;
    ssize_t operator()(unsigned char *b, size_t n) const { return ::recv(fd, b, n, 0); }
};

Connection::Connection() : fd_(-1) {}
Connection::~Connection() { close(); }

void Connection::adopt(int fd) {
    close();
    fd_ = fd;
}

void Connection::close() {
    for (int i = 0; i < NUM_CATEGORIES; i++) {
        queues_[i].clear();
    }
    if (fd_ != -1) {
        close_socket(fd_);
    }
    fd_ = -1;
}

bool Connection::send(const NetworkMessage &msg) {
    if (fd_ == -1) {
        return false;
    }
    if (wclog_level() >= 3) {
        wclog(3, "SEND %s", msg.DebugString().c_str());
    }
    std::string out = "XXX";
    if (!msg.AppendToString(&out)) {
        wclog(0, "protobuf serialize failed");
        return false;
    }
    size_t len = out.length() - 3;
    if (len == 0 || len >= (1u << 24)) {
        wclog(0, "protobuf message size %d out of range", (int)len);
        return false;
    }
    out[0] = (char)(len >> 16);
    out[1] = (char)(len >> 8);
    out[2] = (char)len;
    SendFn fn = { fd_ };
    if (xfer_all(fn, (unsigned char *)&out[0], out.length()) < (ssize_t)out.length()) {
        perror("wcnet send failed");
        return false;
    }
    return true;
}

bool Connection::socket_readable() const {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd_, &set);
    struct timeval tv = { 0, 0 };
    return select(fd_ + 1, &set, NULL, NULL, &tv) > 0;
}

RecvStatus Connection::read_one(NetworkMessage &msg, bool blocking) {
    if (fd_ == -1) {
        return RecvStatus::STATUS_FAIL;
    }
    if (!blocking && !socket_readable()) {
        return RecvStatus::STATUS_NO_DATA;
    }
    unsigned char lengthData[3];
    RecvFn fn = { fd_ };
    ssize_t ret = xfer_all(fn, lengthData, sizeof(lengthData));
    if (ret < (ssize_t)sizeof(lengthData)) {
        if (ret < 0) {
            perror("wcnet recv length failed");
        } else {
            wclog(1, "connection closed by peer");
        }
        return RecvStatus::STATUS_FAIL;
    }
    size_t len = ((size_t)lengthData[0] << 16) | ((size_t)lengthData[1] << 8) | lengthData[2];
    if (len == 0) {
        wclog(0, "empty message received");
        return RecvStatus::STATUS_FAIL;
    }
    std::vector<unsigned char> data(len);
    ret = xfer_all(fn, &data[0], len);
    if (ret < (ssize_t)len) {
        if (ret < 0) {
            perror("wcnet recv data failed");
        } else {
            wclog(1, "connection closed by peer mid-message");
        }
        return RecvStatus::STATUS_FAIL;
    }
    if (!msg.ParseFromArray(&data[0], (int)len)) {
        wclog(0, "protobuf parse failed");
        return RecvStatus::STATUS_FAIL;
    }
    if (wclog_level() >= 3) {
        wclog(3, "RECV %s", msg.DebugString().c_str());
    }
    return RecvStatus::STATUS_OK;
}

RecvStatus Connection::recv(MessageCategory cat, NetworkMessage &msg) {
    if (!queues_[cat].empty()) {
        msg = queues_[cat].front();
        queues_[cat].pop_front();
        return RecvStatus::STATUS_OK;
    }
    while (true) {
        RecvStatus st = read_one(msg, true);
        if (!st.ok()) {
            return st;
        }
        MessageCategory got = category_of(msg);
        if (got == cat) {
            return RecvStatus::STATUS_OK;
        }
        queues_[got].push_back(msg);
    }
}

RecvStatus Connection::poll(MessageCategory cat, NetworkMessage &msg) {
    if (!queues_[cat].empty()) {
        msg = queues_[cat].front();
        queues_[cat].pop_front();
        return RecvStatus::STATUS_OK;
    }
    RecvStatus st = read_one(msg, false);
    if (!st.ok()) {
        return st;
    }
    MessageCategory got = category_of(msg);
    if (got == cat) {
        return RecvStatus::STATUS_OK;
    }
    queues_[got].push_back(msg);
    return RecvStatus::STATUS_NO_DATA;
}

const NetworkMessage *Connection::peek(MessageCategory cat) const {
    if (queues_[cat].empty()) {
        return NULL;
    }
    return &queues_[cat].front();
}

void close_socket(int fd) {
    ::close(fd);
}

int listen_on(const char *portstr) {
    struct addrinfo hints, *res0 = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    int err = getaddrinfo(NULL, portstr, &hints, &res0);
    if (err) {
        wclog(0, "getaddrinfo: %s", gai_strerror(err));
        return -1;
    }
    int s = -1;
    for (struct addrinfo *res = res0; res; res = res->ai_next) {
        s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (s < 0) {
            continue;
        }
        int enable = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
        if (bind(s, res->ai_addr, res->ai_addrlen) < 0 || listen(s, 5) < 0) {
            perror("wcnet bind/listen");
            ::close(s);
            s = -1;
            continue;
        }
        break;
    }
    freeaddrinfo(res0);
    return s;
}

int accept_connection(int listenFd, bool blocking) {
    int flags = fcntl(listenFd, F_GETFL, 0);
    fcntl(listenFd, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
    sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);
    int s = accept(listenFd, (sockaddr *)&addr, &addrlen);
    fcntl(listenFd, F_SETFL, flags & ~O_NONBLOCK);
    if (s < 0) {
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            perror("wcnet accept failed");
        }
        return -1;
    }
    return s;
}

int connect_to(const char *host, const char *portstr) {
    struct addrinfo hints, *res0 = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int err = getaddrinfo(host, portstr, &hints, &res0);
    if (err) {
        wclog(0, "getaddrinfo %s: %s", host, gai_strerror(err));
        return -1;
    }
    int s = -1;
    for (struct addrinfo *res = res0; res; res = res->ai_next) {
        s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (s < 0) {
            continue;
        }
        if (connect(s, res->ai_addr, res->ai_addrlen) < 0) {
            ::close(s);
            s = -1;
            continue;
        }
        break;
    }
    freeaddrinfo(res0);
    return s;
}

}  // namespace wc
