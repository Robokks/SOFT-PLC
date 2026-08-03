#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>

#include "nanomodbus.h"
#include "softplc/net/tcp_listener.hpp"
#include "softplc/net/tcp_socket.hpp"

namespace softplc::test {

// Test-only Modbus TCP server built on nanoMODBUS's own server mode (not a
// hand-rolled byte-level mock) so tests exercise the same real, spec-correct
// protocol implementation the production client uses. Serves one connection at a
// time on a background thread; a new connection is accepted once the previous one
// ends, so client reconnect scenarios work naturally.
class MockModbusServer {
public:
    MockModbusServer();
    ~MockModbusServer();

    MockModbusServer(const MockModbusServer&) = delete;
    MockModbusServer& operator=(const MockModbusServer&) = delete;

    // Binds an ephemeral port on 127.0.0.1 and starts the accept loop. No-op if
    // already started.
    void start();
    // Stops the accept loop and joins it. No-op if not started.
    void stop();

    [[nodiscard]] std::uint16_t port() const { return listener_.boundPort(); }

    void setCoil(std::uint16_t address, bool value);
    [[nodiscard]] bool getCoil(std::uint16_t address) const;
    void setDiscreteInput(std::uint16_t address, bool value);
    void setHoldingRegister(std::uint16_t address, std::uint16_t value);
    [[nodiscard]] std::uint16_t getHoldingRegister(std::uint16_t address) const;
    void setInputRegister(std::uint16_t address, std::uint16_t value);

    // Fault injection, applied to the response (write) side of the next
    // transaction(s). nanoMODBUS itself has no fault-injection hooks -- this is our
    // own layer around its transport callbacks.
    void setResponseDelay(std::chrono::milliseconds delay);
    void setDropNextNResponses(int n);
    // One-shot: closes the connection instead of sending the next response.
    void closeConnectionOnNextRequest();
    void rejectConnections(bool reject);

private:
    // Deliberately smaller than the full uint16 address range (unlike a real device,
    // which would also have a finite address space) so tests can construct a request
    // that's valid client-side (address+quantity fits in range/limits nanoMODBUS
    // itself enforces before ever sending) but still exceeds what this mock server
    // supports, to exercise a genuine round-tripped Modbus exception response rather
    // than a client-side rejection.
    static constexpr std::uint32_t kAddressSpace = 1000;

    void acceptLoop(std::stop_token stopToken);
    void serveConnection(net::TcpSocket socket, std::stop_token stopToken);

    // nmbs_callbacks register-access callbacks (arg == this). Signatures must match
    // nmbs_callbacks's function pointer members exactly (nmbs_bitfield decays to
    // uint8_t* as a parameter, same as in nanomodbus.h itself).
    static nmbs_error onReadCoils(std::uint16_t address, std::uint16_t quantity, nmbs_bitfield coilsOut,
                                   std::uint8_t unitId, void* arg);
    static nmbs_error onReadDiscreteInputs(std::uint16_t address, std::uint16_t quantity,
                                            nmbs_bitfield inputsOut, std::uint8_t unitId, void* arg);
    static nmbs_error onReadHoldingRegisters(std::uint16_t address, std::uint16_t quantity,
                                              std::uint16_t* registersOut, std::uint8_t unitId, void* arg);
    static nmbs_error onReadInputRegisters(std::uint16_t address, std::uint16_t quantity,
                                            std::uint16_t* registersOut, std::uint8_t unitId, void* arg);
    static nmbs_error onWriteSingleCoil(std::uint16_t address, bool value, std::uint8_t unitId, void* arg);
    static nmbs_error onWriteSingleRegister(std::uint16_t address, std::uint16_t value,
                                             std::uint8_t unitId, void* arg);
    static nmbs_error onWriteMultipleCoils(std::uint16_t address, std::uint16_t quantity,
                                            const nmbs_bitfield coils, std::uint8_t unitId, void* arg);
    static nmbs_error onWriteMultipleRegisters(std::uint16_t address, std::uint16_t quantity,
                                                const std::uint16_t* registers, std::uint8_t unitId,
                                                void* arg);

    // nmbs_platform_conf transport callbacks (arg == a per-connection context).
    static std::int32_t transportRead(std::uint8_t* buf, std::uint16_t count, std::int32_t byteTimeoutMs,
                                       void* arg);
    static std::int32_t transportWrite(const std::uint8_t* buf, std::uint16_t count,
                                        std::int32_t byteTimeoutMs, void* arg);

    [[nodiscard]] bool takeDropSignal();
    [[nodiscard]] bool takeCloseSignal();
    [[nodiscard]] std::chrono::milliseconds responseDelay() const;

    net::TcpListener listener_;
    std::jthread acceptThread_;

    mutable std::mutex registersMutex_;
    std::array<bool, kAddressSpace> coils_{};
    std::array<bool, kAddressSpace> discreteInputs_{};
    std::array<std::uint16_t, kAddressSpace> holdingRegisters_{};
    std::array<std::uint16_t, kAddressSpace> inputRegisters_{};

    std::atomic<bool> rejectConnections_{false};
    std::atomic<int> dropNextN_{0};
    std::atomic<bool> closeOnNextRequest_{false};
    std::atomic<std::int64_t> responseDelayMs_{0};
};

}  // namespace softplc::test
