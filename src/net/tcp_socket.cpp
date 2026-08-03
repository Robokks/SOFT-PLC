#include "softplc/net/tcp_socket.hpp"

#include <cerrno>
#include <cstring>
#include <string>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
using SockLen = int;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
using SockLen = socklen_t;
constexpr socket_t kInvalidSocket = -1;
#endif

namespace softplc::net {

namespace {

#if defined(_WIN32)
struct WinsockGuard {
    WinsockGuard() {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    }
    ~WinsockGuard() { WSACleanup(); }
};

void ensureWinsockInitialized() {
    static WinsockGuard guard;
    (void)guard;
}
#endif

socket_t toNative(std::intptr_t handle) { return static_cast<socket_t>(handle); }
std::intptr_t fromNative(socket_t s) { return static_cast<std::intptr_t>(s); }

void closeNative(socket_t s) {
#if defined(_WIN32)
    closesocket(s);
#else
    ::close(s);
#endif
}

bool setNonBlocking(socket_t s, bool nonBlocking) {
#if defined(_WIN32)
    u_long mode = nonBlocking ? 1 : 0;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    const int updated = nonBlocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(s, F_SETFL, updated) == 0;
#endif
}

void setRecvTimeout(socket_t s, std::chrono::milliseconds timeout) {
#if defined(_WIN32)
    DWORD ms = static_cast<DWORD>(timeout.count());
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
    struct timeval tv;
    tv.tv_sec = static_cast<time_t>(timeout.count() / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

void setSendTimeout(socket_t s, std::chrono::milliseconds timeout) {
#if defined(_WIN32)
    DWORD ms = static_cast<DWORD>(timeout.count());
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
    struct timeval tv;
    tv.tv_sec = static_cast<time_t>(timeout.count() / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

bool wasTimeout() {
#if defined(_WIN32)
    const int err = WSAGetLastError();
    return err == WSAETIMEDOUT || err == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

enum class RawOutcome { Data, Timeout, Closed, Error };
struct RawResult {
    RawOutcome outcome;
    int bytes = 0;
};

RawResult recvOnce(socket_t s, char* buf, int len, std::chrono::milliseconds timeout) {
    setRecvTimeout(s, timeout);
    const int n = static_cast<int>(recv(s, buf, len, 0));
    if (n > 0) return {RawOutcome::Data, n};
    if (n == 0) return {RawOutcome::Closed, 0};
    if (wasTimeout()) return {RawOutcome::Timeout, 0};
    return {RawOutcome::Error, 0};
}

RawResult sendOnce(socket_t s, const char* buf, int len, std::chrono::milliseconds timeout) {
    setSendTimeout(s, timeout);
    const int n = static_cast<int>(send(s, buf, len, 0));
    if (n >= 0) return {RawOutcome::Data, n};
    if (wasTimeout()) return {RawOutcome::Timeout, 0};
    return {RawOutcome::Error, 0};
}

}  // namespace

const char* toString(SocketError error) {
    switch (error) {
        case SocketError::None:
            return "None";
        case SocketError::ConnectFailed:
            return "ConnectFailed";
        case SocketError::ConnectTimeout:
            return "ConnectTimeout";
        case SocketError::SendFailed:
            return "SendFailed";
        case SocketError::RecvTimeout:
            return "RecvTimeout";
        case SocketError::RecvFailed:
            return "RecvFailed";
        case SocketError::ConnectionClosed:
            return "ConnectionClosed";
        case SocketError::NotConnected:
            return "NotConnected";
    }
    return "Unknown";
}

TcpSocket::TcpSocket() : handle_(-1) {
#if defined(_WIN32)
    ensureWinsockInitialized();
#endif
}

TcpSocket::TcpSocket(std::intptr_t handle) : handle_(handle) {
#if defined(_WIN32)
    ensureWinsockInitialized();
#endif
}

TcpSocket::~TcpSocket() { close(); }

TcpSocket::TcpSocket(TcpSocket&& other) noexcept : handle_(other.handle_) { other.handle_ = -1; }

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = -1;
    }
    return *this;
}

TcpSocket TcpSocket::adopt(std::intptr_t nativeHandle) { return TcpSocket(nativeHandle); }

bool TcpSocket::isOpen() const { return handle_ != -1; }

void TcpSocket::close() {
    if (handle_ != -1) {
        closeNative(toNative(handle_));
        handle_ = -1;
    }
}

SocketError TcpSocket::connect(std::string_view host, std::uint16_t port,
                                std::chrono::milliseconds timeout) {
    close();

    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = nullptr;
    const std::string hostStr(host);
    const std::string portStr = std::to_string(port);
    if (getaddrinfo(hostStr.c_str(), portStr.c_str(), &hints, &result) != 0 || result == nullptr) {
        return SocketError::ConnectFailed;
    }

    SocketError lastResult = SocketError::ConnectFailed;
    for (struct addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
        const socket_t s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (s == kInvalidSocket) continue;

        setNonBlocking(s, true);
        const int rc = ::connect(s, rp->ai_addr, static_cast<int>(rp->ai_addrlen));
        if (rc == 0) {
            setNonBlocking(s, false);
            handle_ = fromNative(s);
            freeaddrinfo(result);
            return SocketError::None;
        }

#if defined(_WIN32)
        const bool inProgress = (WSAGetLastError() == WSAEWOULDBLOCK);
#else
        const bool inProgress = (errno == EINPROGRESS);
#endif
        if (!inProgress) {
            closeNative(s);
            lastResult = SocketError::ConnectFailed;
            continue;
        }

        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(s, &writeSet);
        struct timeval tv;
        tv.tv_sec = static_cast<long>(timeout.count() / 1000);
        tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);

#if defined(_WIN32)
        const int selectRc = select(0, nullptr, &writeSet, nullptr, &tv);
#else
        const int selectRc = select(static_cast<int>(s) + 1, nullptr, &writeSet, nullptr, &tv);
#endif
        if (selectRc <= 0) {
            closeNative(s);
            lastResult = SocketError::ConnectTimeout;
            continue;
        }

        int soError = 0;
        SockLen soErrorLen = sizeof(soError);
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &soErrorLen) != 0 ||
            soError != 0) {
            closeNative(s);
            lastResult = SocketError::ConnectFailed;
            continue;
        }

        setNonBlocking(s, false);
        handle_ = fromNative(s);
        freeaddrinfo(result);
        return SocketError::None;
    }

    freeaddrinfo(result);
    return lastResult;
}

std::int32_t TcpSocket::recvSome(std::span<std::byte> buffer, std::chrono::milliseconds timeout) {
    // Loops internally until buffer.size() bytes are read or the overall timeout
    // elapses (a single recv() call only waits for *some* data, not a specific
    // count) -- this is the contract third-party libraries like nanoMODBUS require
    // of a transport read callback. Returns the byte count actually transferred,
    // which is a valid partial result on timeout; -1 only on a hard transport error
    // (including an orderly close that yielded zero bytes).
    if (handle_ == -1) return -1;
    if (buffer.empty()) return 0;
    const socket_t s = toNative(handle_);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::size_t received = 0;
    while (received < buffer.size()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const RawResult result =
            recvOnce(s, reinterpret_cast<char*>(buffer.data() + received),
                     static_cast<int>(buffer.size() - received), remaining);
        switch (result.outcome) {
            case RawOutcome::Data:
                received += static_cast<std::size_t>(result.bytes);
                break;
            case RawOutcome::Timeout:
                return static_cast<std::int32_t>(received);
            case RawOutcome::Closed:
                return received > 0 ? static_cast<std::int32_t>(received) : -1;
            case RawOutcome::Error:
                return -1;
        }
    }
    return static_cast<std::int32_t>(received);
}

std::int32_t TcpSocket::sendSome(std::span<const std::byte> data, std::chrono::milliseconds timeout) {
    // Same looping contract as recvSome(), for the transport write callback side.
    if (handle_ == -1) return -1;
    if (data.empty()) return 0;
    const socket_t s = toNative(handle_);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const RawResult result = sendOnce(s, reinterpret_cast<const char*>(data.data() + sent),
                                           static_cast<int>(data.size() - sent), remaining);
        if (result.outcome == RawOutcome::Error) return sent > 0 ? static_cast<std::int32_t>(sent) : -1;
        if (result.outcome == RawOutcome::Timeout) return static_cast<std::int32_t>(sent);
        sent += static_cast<std::size_t>(result.bytes);
    }
    return static_cast<std::int32_t>(sent);
}

SocketError TcpSocket::recvExact(std::span<std::byte> buffer, std::chrono::milliseconds timeout) {
    if (handle_ == -1) return SocketError::NotConnected;
    const socket_t s = toNative(handle_);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::size_t received = 0;
    while (received < buffer.size()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return SocketError::RecvTimeout;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const RawResult result =
            recvOnce(s, reinterpret_cast<char*>(buffer.data() + received),
                     static_cast<int>(buffer.size() - received), remaining);
        switch (result.outcome) {
            case RawOutcome::Data:
                received += static_cast<std::size_t>(result.bytes);
                break;
            case RawOutcome::Timeout:
                return SocketError::RecvTimeout;
            case RawOutcome::Closed:
                return SocketError::ConnectionClosed;
            case RawOutcome::Error:
                return SocketError::RecvFailed;
        }
    }
    return SocketError::None;
}

SocketError TcpSocket::send(std::span<const std::byte> data, std::chrono::milliseconds timeout) {
    if (handle_ == -1) return SocketError::NotConnected;
    const socket_t s = toNative(handle_);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return SocketError::SendFailed;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const RawResult result = sendOnce(s, reinterpret_cast<const char*>(data.data() + sent),
                                           static_cast<int>(data.size() - sent), remaining);
        if (result.outcome == RawOutcome::Error) return SocketError::SendFailed;
        sent += static_cast<std::size_t>(result.bytes);
    }
    return SocketError::None;
}

}  // namespace softplc::net
