#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "softplc/io/modbus_client.hpp"
#include "softplc/io/modbus_mapping.hpp"
#include "softplc/net/serial_port.hpp"

namespace softplc::io {

// One physical Modbus RTU bus: owns a single net::SerialPort and a single vendored
// nanoMODBUS client instance, shared across every unit id/device configured on that
// bus. Unlike ModbusClient (one TCP connection == one device), a serial line is a
// shared half-duplex medium, so there is exactly one transport per bus; each method
// takes the target unitId explicitly and sets it via nmbs_set_destination_rtu_address()
// before issuing the request (see modbus_rtu_bus.cpp). Not thread-safe and not
// movable/copyable -- owned exclusively by one bus's poller thread
// (ModbusRtuIoDriver). Any I/O error (other than a well-formed Modbus exception
// response) closes the port so the next call reopens it from scratch, mirroring
// ModbusClient's reconnect convention.
class ModbusRtuBus {
public:
    explicit ModbusRtuBus(ModbusRtuBusConfig config);
    ~ModbusRtuBus();

    ModbusRtuBus(const ModbusRtuBus&) = delete;
    ModbusRtuBus& operator=(const ModbusRtuBus&) = delete;
    ModbusRtuBus(ModbusRtuBus&&) = delete;
    ModbusRtuBus& operator=(ModbusRtuBus&&) = delete;

    [[nodiscard]] bool isOpen() const;
    RequestResult ensureOpen();
    void close();

    RequestResult readBatch(std::uint8_t unitId, const ReadBatch& batch,
                             std::vector<std::uint16_t>& registersOut, std::vector<bool>& bitsOut);
    RequestResult writeRegisters(std::uint8_t unitId, std::uint16_t address,
                                  const std::vector<std::uint16_t>& values);
    RequestResult writeCoils(std::uint8_t unitId, std::uint16_t address, const std::vector<bool>& values);

private:
    ModbusRtuBusConfig config_;
    net::SerialPort port_;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace softplc::io
