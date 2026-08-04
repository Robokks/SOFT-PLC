#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "nanomodbus.h"
#include "softplc/net/serial_port.hpp"

namespace softplc::test {

// Test-only Modbus RTU server, built on nanoMODBUS's own server mode over a real
// pseudo-terminal pair (so client-side production code exercises genuine serial
// framing/CRC through net::SerialPort, not a hand-rolled mock). Responds only to
// requests addressed to `unitAddress()` -- exactly like real hardware sharing a
// multi-drop bus, and the mechanism this project's tests use to prove
// ModbusRtuBus really sends the unit id it's given (a request to any other unit id
// gets no response at all, i.e. a client-side timeout).
class MockModbusRtuServer {
public:
    explicit MockModbusRtuServer(std::uint8_t unitAddress = 5);
    ~MockModbusRtuServer();

    MockModbusRtuServer(const MockModbusRtuServer&) = delete;
    MockModbusRtuServer& operator=(const MockModbusRtuServer&) = delete;

    // Starts the serve loop on a background thread. No-op if already started.
    void start();
    // Stops the serve loop and joins it. No-op if not started.
    void stop();

    [[nodiscard]] const std::string& slavePath() const { return slavePath_; }
    [[nodiscard]] std::uint8_t unitAddress() const { return unitAddress_; }

    void setCoil(std::uint16_t address, bool value);
    [[nodiscard]] bool getCoil(std::uint16_t address) const;
    void setDiscreteInput(std::uint16_t address, bool value);
    void setHoldingRegister(std::uint16_t address, std::uint16_t value);
    [[nodiscard]] std::uint16_t getHoldingRegister(std::uint16_t address) const;
    void setInputRegister(std::uint16_t address, std::uint16_t value);

private:
    static constexpr std::uint32_t kAddressSpace = 1000;

    void serveLoop(std::stop_token stopToken);

    static nmbs_error onReadCoils(std::uint16_t address, std::uint16_t quantity, nmbs_bitfield coilsOut,
                                   std::uint8_t unitId, void* arg);
    static nmbs_error onReadDiscreteInputs(std::uint16_t address, std::uint16_t quantity,
                                            nmbs_bitfield inputsOut, std::uint8_t unitId, void* arg);
    static nmbs_error onReadHoldingRegisters(std::uint16_t address, std::uint16_t quantity,
                                              std::uint16_t* registersOut, std::uint8_t unitId, void* arg);
    static nmbs_error onReadInputRegisters(std::uint16_t address, std::uint16_t quantity,
                                            std::uint16_t* registersOut, std::uint8_t unitId, void* arg);
    static nmbs_error onWriteMultipleCoils(std::uint16_t address, std::uint16_t quantity,
                                            const nmbs_bitfield coils, std::uint8_t unitId, void* arg);
    static nmbs_error onWriteMultipleRegisters(std::uint16_t address, std::uint16_t quantity,
                                                const std::uint16_t* registers, std::uint8_t unitId,
                                                void* arg);

    static std::int32_t transportRead(std::uint8_t* buf, std::uint16_t count, std::int32_t byteTimeoutMs,
                                       void* arg);
    static std::int32_t transportWrite(const std::uint8_t* buf, std::uint16_t count,
                                        std::int32_t byteTimeoutMs, void* arg);

    int masterFd_ = -1;
    std::string slavePath_;
    std::uint8_t unitAddress_;
    net::SerialPort masterPort_;
    std::jthread thread_;

    mutable std::mutex registersMutex_;
    std::array<bool, kAddressSpace> coils_{};
    std::array<bool, kAddressSpace> discreteInputs_{};
    std::array<std::uint16_t, kAddressSpace> holdingRegisters_{};
    std::array<std::uint16_t, kAddressSpace> inputRegisters_{};
};

}  // namespace softplc::test
