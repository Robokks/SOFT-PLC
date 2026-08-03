#pragma once

#include <chrono>
#include <cstdint>
#include <span>
#include <string_view>

namespace softplc::net {

enum class SocketError {
    None,
    ConnectFailed,
    ConnectTimeout,
    SendFailed,
    RecvTimeout,
    RecvFailed,
    ConnectionClosed,
    NotConnected,
};

const char* toString(SocketError error);

// Minimal cross-platform blocking TCP client socket with connect/send/recv timeouts.
// Movable, not copyable. Not thread-safe: an instance is meant to be owned and used
// by a single thread (e.g. one Modbus device's poller thread).
class TcpSocket {
public:
    TcpSocket();
    ~TcpSocket();

    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;

    [[nodiscard]] SocketError connect(std::string_view host, std::uint16_t port,
                                       std::chrono::milliseconds timeout);

    // All-or-nothing convenience wrappers: succeed only if the full buffer is
    // transferred within the timeout.
    [[nodiscard]] SocketError send(std::span<const std::byte> data, std::chrono::milliseconds timeout);
    [[nodiscard]] SocketError recvExact(std::span<std::byte> buffer, std::chrono::milliseconds timeout);

    // Lower-level byte-count primitives: block until at least one byte transfers or
    // the timeout elapses, looping internally up to the full buffer size or timeout,
    // and return the number of bytes actually transferred (may be less than the
    // buffer size on timeout — that's a valid, non-error outcome here). Returns a
    // negative value only on a hard transport error. This matches the contract
    // required by third-party libraries (e.g. nanoMODBUS's platform transport
    // callbacks); recvExact()/send() are implemented on top of these.
    [[nodiscard]] std::int32_t recvSome(std::span<std::byte> buffer, std::chrono::milliseconds timeout);
    [[nodiscard]] std::int32_t sendSome(std::span<const std::byte> data, std::chrono::milliseconds timeout);

    void close();
    [[nodiscard]] bool isOpen() const;

    // Native socket handle, exposed for the mock test server's accept()-based use.
    // -1 when not open. Not part of the stable public contract for driver code.
    [[nodiscard]] std::intptr_t nativeHandle() const { return handle_; }

    // Wraps an already-connected/accepted native handle. Takes ownership (will
    // close() it on destruction). Used only by the test-only mock server.
    static TcpSocket adopt(std::intptr_t nativeHandle);

private:
    explicit TcpSocket(std::intptr_t handle);

    std::intptr_t handle_;
};

}  // namespace softplc::net
