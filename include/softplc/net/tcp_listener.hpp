#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

#include "softplc/net/tcp_socket.hpp"

namespace softplc::net {

// Minimal cross-platform TCP listening socket. Server-side/test-support use only —
// production Modbus TCP in this project is client-only and never needs this. Kept in
// `net/` rather than test-only code since it's a natural counterpart to TcpSocket for
// any future server-role work (e.g. a Modbus/OPC-UA server exposing TagStore).
class TcpListener {
public:
    TcpListener();
    ~TcpListener();

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;
    TcpListener(TcpListener&& other) noexcept;
    TcpListener& operator=(TcpListener&& other) noexcept;

    // Binds to 127.0.0.1:port (port 0 picks an ephemeral port) and starts listening.
    [[nodiscard]] bool listen(std::uint16_t port, int backlog = 4);
    [[nodiscard]] std::uint16_t boundPort() const;

    // Waits up to `timeout` for an incoming connection; nullopt on timeout or error.
    [[nodiscard]] std::optional<TcpSocket> accept(std::chrono::milliseconds timeout);

    void close();
    [[nodiscard]] bool isOpen() const;

private:
    std::intptr_t handle_;
};

}  // namespace softplc::net
