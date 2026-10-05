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
#include "wcnet_perf.h"
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

// ---------------------------------------------------------------------------
// Connection: protobuf (de)serialization and per-category queues

Connection::Connection() : stream_(NULL) {}
Connection::~Connection() { close(); }

void Connection::adopt(Stream *stream) {
    close();
    stream_ = stream;
}

void Connection::close() {
    for (int i = 0; i < NUM_CATEGORIES; i++) {
        queues_[i].clear();
    }
    held_.clear();
    heldFailed_ = false;
    if (stream_) {
        stream_->close();
        delete stream_;
        stream_ = NULL;
    }
}

bool Connection::send(const NetworkMessage &msg) {
    if (!stream_) {
        return false;
    }
    if (wclog_level() >= 3) {
        wclog(3, "SEND %s", msg.DebugString().c_str());
    }
    std::string out;
    if (!msg.SerializeToString(&out)) {
        wclog(0, "protobuf serialize failed");
        return false;
    }
    if (out.empty()) {
        wclog(0, "refusing to send an empty message");
        return false;
    }
    return stream_->send(out);
}

// Test aid: WCNET_LAG=<ms> (native builds) holds every message that comes in
// back for that long, as a slow link would, one way; with it on both
// machines the round trip is twice that.
static double lag_ms() {
    static double lag = -1;
    if (lag < 0) {
#ifdef __EMSCRIPTEN__
        lag = 0;
#else
        const char *env = getenv("WCNET_LAG");
        lag = env && env[0] ? atof(env) : 0;
#endif
    }
    return lag;
}

RecvStatus Connection::read_raw(std::string &data, bool blocking) {
    double lag = lag_ms();
    if (lag <= 0) {
        return stream_->recv(data, blocking);
    }
#ifndef __EMSCRIPTEN__
    while (!heldFailed_) {
        std::string in;
        RecvStatus st = stream_->recv(in, false);
        if (st.ok()) {
            held_.push_back(std::make_pair(perf_now_ms() + lag, in));
        } else {
            heldFailed_ = st.failed();
            break;
        }
    }
    if (held_.empty()) {
        if (heldFailed_) {
            return RecvStatus::STATUS_FAIL;
        }
        if (!blocking) {
            return RecvStatus::STATUS_NO_DATA;
        }
        std::string in;
        RecvStatus st = stream_->recv(in, true);
        if (!st.ok()) {
            return st;
        }
        held_.push_back(std::make_pair(perf_now_ms() + lag, in));
    }
    double wait = held_.front().first - perf_now_ms();
    if (wait > 0) {
        if (!blocking) {
            return RecvStatus::STATUS_NO_DATA;
        }
        usleep((useconds_t)(wait * 1000.0));
    }
    data = held_.front().second;
    held_.pop_front();
#endif
    return RecvStatus::STATUS_OK;
}

RecvStatus Connection::read_one(NetworkMessage &msg, bool blocking) {
    if (!stream_) {
        return RecvStatus::STATUS_FAIL;
    }
    std::string data;
    RecvStatus st = read_raw(data, blocking);
    if (!st.ok()) {
        return st;
    }
    if (!msg.ParseFromArray(data.data(), (int)data.size())) {
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
    // Blocking: the time this takes is what a frame waits for the others.
    double began = perf_now_ms();
    while (true) {
        RecvStatus st = read_one(msg, true);
        if (!st.ok()) {
            return st;
        }
        MessageCategory got = category_of(msg);
        if (got == cat) {
            perf_wait(perf_now_ms() - began);
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
    // Everything that has arrived is read, until one of `cat` turns up: a
    // chat line in front of a frame must not hide the frame.
    while (true) {
        RecvStatus st = read_one(msg, false);
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

const NetworkMessage *Connection::peek(MessageCategory cat) const {
    if (queues_[cat].empty()) {
        return NULL;
    }
    return &queues_[cat].front();
}

// ---------------------------------------------------------------------------
// TCP: 3-byte big-endian length prefix per message

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

static std::string peer_name(int fd) {
    sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    char host[NI_MAXHOST], port[NI_MAXSERV];
    if (getpeername(fd, (sockaddr *)&addr, &len) == 0 &&
        getnameinfo((sockaddr *)&addr, len, host, sizeof(host), port, sizeof(port),
                    NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
        return std::string("tcp ") + host + ":" + port;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "tcp fd %d", fd);
    return buf;
}

class TcpStream : public Stream {
public:
    explicit TcpStream(int fd) : fd_(fd), name_(peer_name(fd)) {}
    ~TcpStream() { close(); }

    virtual bool is_open() const { return fd_ != -1; }

    virtual void close() {
        if (fd_ != -1) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    virtual bool send(const std::string &bytes) {
        if (fd_ == -1) {
            return false;
        }
        size_t len = bytes.length();
        if (len == 0 || len >= (1u << 24)) {
            wclog(0, "message size %d out of range for tcp framing", (int)len);
            return false;
        }
        std::string out;
        out.reserve(len + 3);
        out.push_back((char)(len >> 16));
        out.push_back((char)(len >> 8));
        out.push_back((char)len);
        out += bytes;
        SendFn fn = { fd_ };
        if (xfer_all(fn, (unsigned char *)&out[0], out.length()) < (ssize_t)out.length()) {
            perror("wcnet send failed");
            return false;
        }
        return true;
    }

    virtual RecvStatus recv(std::string &bytes, bool blocking) {
        if (fd_ == -1) {
            return RecvStatus::STATUS_FAIL;
        }
        if (!blocking && !readable()) {
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
        bytes.resize(len);
        ret = xfer_all(fn, (unsigned char *)&bytes[0], len);
        if (ret < (ssize_t)len) {
            if (ret < 0) {
                perror("wcnet recv data failed");
            } else {
                wclog(1, "connection closed by peer mid-message");
            }
            return RecvStatus::STATUS_FAIL;
        }
        return RecvStatus::STATUS_OK;
    }

    virtual std::string describe() const { return name_; }

private:
    bool readable() const {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd_, &set);
        struct timeval tv = { 0, 0 };
        return select(fd_ + 1, &set, NULL, NULL, &tv) > 0;
    }

    int fd_;
    std::string name_;
};

class TcpListener : public Listener {
public:
    TcpListener(int fd, const std::string &portstr) : fd_(fd), portstr_(portstr) {}
    ~TcpListener() { ::close(fd_); }

    virtual Stream *accept(bool blocking) {
        int flags = fcntl(fd_, F_GETFL, 0);
        fcntl(fd_, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
        sockaddr_storage addr;
        socklen_t addrlen = sizeof(addr);
        int s = ::accept(fd_, (sockaddr *)&addr, &addrlen);
        fcntl(fd_, F_SETFL, flags & ~O_NONBLOCK);
        if (s < 0) {
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                perror("wcnet accept failed");
            }
            return NULL;
        }
        return new TcpStream(s);
    }

    virtual std::string describe() const { return "tcp port " + portstr_; }

private:
    int fd_;
    std::string portstr_;
};

Listener *tcp_listen(const char *portstr) {
    struct addrinfo hints, *res0 = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    int err = getaddrinfo(NULL, portstr, &hints, &res0);
    if (err) {
        wclog(0, "getaddrinfo: %s", gai_strerror(err));
        return NULL;
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
    if (s < 0) {
        return NULL;
    }
    return new TcpListener(s, portstr);
}

Stream *tcp_connect(const char *host, const char *portstr) {
    struct addrinfo hints, *res0 = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int err = getaddrinfo(host, portstr, &hints, &res0);
    if (err) {
        wclog(0, "getaddrinfo %s: %s", host, gai_strerror(err));
        return NULL;
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
    if (s < 0) {
        return NULL;
    }
    return new TcpStream(s);
}

}  // namespace wc
