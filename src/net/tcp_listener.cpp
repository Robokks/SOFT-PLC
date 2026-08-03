#include "softplc/net/tcp_listener.hpp"

#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
using SockLen = int;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
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

}  // namespace

TcpListener::TcpListener() : handle_(-1) {
#if defined(_WIN32)
    ensureWinsockInitialized();
#endif
}

TcpListener::~TcpListener() { close(); }

TcpListener::TcpListener(TcpListener&& other) noexcept : handle_(other.handle_) {
    other.handle_ = -1;
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = -1;
    }
    return *this;
}

bool TcpListener::isOpen() const { return handle_ != -1; }

void TcpListener::close() {
    if (handle_ != -1) {
        closeNative(toNative(handle_));
        handle_ = -1;
    }
}

bool TcpListener::listen(std::uint16_t port, int backlog) {
    close();

    const socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == kInvalidSocket) return false;

    const int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    if (bind(s, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        closeNative(s);
        return false;
    }
    if (::listen(s, backlog) != 0) {
        closeNative(s);
        return false;
    }

    handle_ = fromNative(s);
    return true;
}

std::uint16_t TcpListener::boundPort() const {
    if (handle_ == -1) return 0;
    struct sockaddr_in addr {};
    SockLen len = sizeof(addr);
    if (getsockname(toNative(handle_), reinterpret_cast<struct sockaddr*>(&addr), &len) != 0) {
        return 0;
    }
    return ntohs(addr.sin_port);
}

std::optional<TcpSocket> TcpListener::accept(std::chrono::milliseconds timeout) {
    if (handle_ == -1) return std::nullopt;
    const socket_t s = toNative(handle_);

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(s, &readSet);
    struct timeval tv;
    tv.tv_sec = static_cast<long>(timeout.count() / 1000);
    tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);

#if defined(_WIN32)
    const int selectRc = select(0, &readSet, nullptr, nullptr, &tv);
#else
    const int selectRc = select(static_cast<int>(s) + 1, &readSet, nullptr, nullptr, &tv);
#endif
    if (selectRc <= 0) return std::nullopt;

    const socket_t clientSocket = ::accept(s, nullptr, nullptr);
    if (clientSocket == kInvalidSocket) return std::nullopt;

    return TcpSocket::adopt(fromNative(clientSocket));
}

}  // namespace softplc::net
