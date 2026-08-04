#pragma once

#include <chrono>
#include <cstdint>
#include <span>
#include <string_view>

namespace softplc::net {

enum class SerialParity { None, Even, Odd };

struct SerialConfig {
    std::uint32_t baudRate = 9600;
    std::uint8_t dataBits = 8;
    SerialParity parity = SerialParity::None;
    std::uint8_t stopBits = 1;
};

enum class SerialError {
    None,
    OpenFailed,
    ConfigFailed,
};

const char* toString(SerialError error);

// Minimal cross-platform blocking serial port with byte-level read/write timeouts.
// Deliberately mirrors net::TcpSocket's recvSome()/sendSome() contract exactly (same
// signatures, same "byte count or -1 on hard error, partial result on timeout is
// valid" semantics) so it plugs into a nanoMODBUS transport callback the same way --
// see io::ModbusRtuBus. Movable, not copyable. Not thread-safe: an instance is meant
// to be owned and used by a single thread (one Modbus RTU bus's poller thread).
class SerialPort {
public:
    SerialPort();
    ~SerialPort();

    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;
    SerialPort(SerialPort&& other) noexcept;
    SerialPort& operator=(SerialPort&& other) noexcept;

    [[nodiscard]] SerialError open(std::string_view devicePath, const SerialConfig& config);

    // Same byte-count/timeout contract as net::TcpSocket::recvSome()/sendSome():
    // loops internally until buffer.size() bytes transfer or the overall timeout
    // elapses, returning the count actually transferred (a valid partial result on
    // timeout) or -1 only on a hard transport error.
    [[nodiscard]] std::int32_t recvSome(std::span<std::byte> buffer, std::chrono::milliseconds timeout);
    [[nodiscard]] std::int32_t sendSome(std::span<const std::byte> data, std::chrono::milliseconds timeout);

    void close();
    [[nodiscard]] bool isOpen() const;

    // Native file/handle, exposed for the mock test server's pseudo-terminal-based
    // use. Not part of the stable public contract for driver code.
    [[nodiscard]] std::intptr_t nativeHandle() const { return handle_; }

    // Wraps an already-open native handle (e.g. a PTY master fd). Takes ownership
    // (will close() it on destruction). Used only by the test-only mock server.
    static SerialPort adopt(std::intptr_t nativeHandle);

private:
    explicit SerialPort(std::intptr_t handle);

    std::intptr_t handle_;
};

}  // namespace softplc::net
