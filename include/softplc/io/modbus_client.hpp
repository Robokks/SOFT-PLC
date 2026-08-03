#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "softplc/io/modbus_mapping.hpp"
#include "softplc/net/tcp_socket.hpp"

namespace softplc::io {

enum class RequestOutcome {
    Success,
    ConnectFailed,
    Timeout,
    ConnectionLost,
    ProtocolError,
    ModbusException,
};

struct RequestResult {
    RequestOutcome outcome = RequestOutcome::Success;
    std::string detail;
    // Valid only when outcome == ModbusException (a Modbus exception code, e.g.
    // illegal data address) -- distinct from a transport/protocol failure: the
    // connection itself is healthy, the device just rejected this specific request.
    std::uint8_t modbusExceptionCode = 0;

    [[nodiscard]] bool ok() const { return outcome == RequestOutcome::Success; }
};

// One Modbus TCP device connection: owns a TcpSocket and a vendored nanoMODBUS client
// instance (kept out of this header via pimpl), translating its calls into
// structured, never-throwing RequestResults. Not thread-safe and not movable/
// copyable -- owned exclusively by one device's poller thread (ModbusTcpIoDriver).
// Any I/O error (other than a well-formed Modbus exception response) closes the
// socket so the next call reconnects from scratch rather than reusing a connection
// that may be left in a desynced state.
class ModbusClient {
public:
    explicit ModbusClient(ModbusDeviceConfig config);
    ~ModbusClient();

    ModbusClient(const ModbusClient&) = delete;
    ModbusClient& operator=(const ModbusClient&) = delete;
    ModbusClient(ModbusClient&&) = delete;
    ModbusClient& operator=(ModbusClient&&) = delete;

    [[nodiscard]] bool isConnected() const;

    // (Re)connects if not already connected. Returns Success (a no-op) if already
    // connected. Also called internally by readBatch()/writeRegisters()/
    // writeCoils(), so callers don't strictly need to call this first -- exposed
    // separately so the driver can probe connectivity without issuing a data request.
    RequestResult ensureConnected();

    void disconnect();

    // Reads a batch (as produced by buildBatches()). Exactly one of
    // registersOut/bitsOut is populated, matching batch.registerType; the other is
    // left untouched.
    RequestResult readBatch(const ReadBatch& batch, std::vector<std::uint16_t>& registersOut,
                             std::vector<bool>& bitsOut);

    // Writes a contiguous run of registers/coils starting at `address`.
    RequestResult writeRegisters(std::uint16_t address, const std::vector<std::uint16_t>& values);
    RequestResult writeCoils(std::uint16_t address, const std::vector<bool>& values);

private:
    ModbusDeviceConfig config_;
    net::TcpSocket socket_;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace softplc::io
