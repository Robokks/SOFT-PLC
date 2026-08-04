#include "softplc/io/modbus_rtu_bus.hpp"

#include <chrono>
#include <cstring>
#include <span>
#include <utility>

#include "modbus_nmbs_error.hpp"
#include "nanomodbus.h"

namespace softplc::io {

namespace {

std::int32_t transportRead(std::uint8_t* buf, std::uint16_t count, std::int32_t byteTimeoutMs, void* arg) {
    auto* port = static_cast<net::SerialPort*>(arg);
    const auto timeout = std::chrono::milliseconds(byteTimeoutMs < 0 ? 60000 : byteTimeoutMs);
    return port->recvSome(std::span<std::byte>(reinterpret_cast<std::byte*>(buf), count), timeout);
}

std::int32_t transportWrite(const std::uint8_t* buf, std::uint16_t count, std::int32_t byteTimeoutMs,
                             void* arg) {
    auto* port = static_cast<net::SerialPort*>(arg);
    const auto timeout = std::chrono::milliseconds(byteTimeoutMs < 0 ? 60000 : byteTimeoutMs);
    return port->sendSome(std::span<const std::byte>(reinterpret_cast<const std::byte*>(buf), count),
                           timeout);
}

}  // namespace

struct ModbusRtuBus::Impl {
    nmbs_t nmbs{};
    bool created = false;
};

ModbusRtuBus::ModbusRtuBus(ModbusRtuBusConfig config)
    : config_(std::move(config)), impl_(std::make_unique<Impl>()) {}

ModbusRtuBus::~ModbusRtuBus() = default;

bool ModbusRtuBus::isOpen() const { return port_.isOpen() && impl_->created; }

void ModbusRtuBus::close() {
    port_.close();
    impl_->created = false;
}

RequestResult ModbusRtuBus::ensureOpen() {
    if (isOpen()) {
        return RequestResult{};
    }
    close();

    const net::SerialError openError = port_.open(config_.devicePath, config_.serial);
    if (openError != net::SerialError::None) {
        RequestResult result;
        result.outcome = RequestOutcome::ConnectFailed;
        result.detail = net::toString(openError);
        return result;
    }

    nmbs_platform_conf conf;
    nmbs_platform_conf_create(&conf);
    conf.transport = NMBS_TRANSPORT_RTU;
    conf.read = transportRead;
    conf.write = transportWrite;
    conf.arg = &port_;

    const nmbs_error rc = nmbs_client_create(&impl_->nmbs, &conf);
    if (rc != NMBS_ERROR_NONE) {
        port_.close();
        RequestResult result;
        result.outcome = RequestOutcome::ConnectFailed;
        result.detail = nmbs_strerror(rc);
        return result;
    }

    const auto timeoutMs = static_cast<std::int32_t>(config_.requestTimeout.count());
    nmbs_set_read_timeout(&impl_->nmbs, timeoutMs);
    nmbs_set_byte_timeout(&impl_->nmbs, timeoutMs);

    impl_->created = true;
    return RequestResult{};
}

RequestResult ModbusRtuBus::readBatch(std::uint8_t unitId, const ReadBatch& batch,
                                       std::vector<std::uint16_t>& registersOut,
                                       std::vector<bool>& bitsOut) {
    RequestResult openResult = ensureOpen();
    if (!openResult.ok()) return openResult;
    nmbs_set_destination_rtu_address(&impl_->nmbs, unitId);

    nmbs_error rc = NMBS_ERROR_NONE;
    switch (batch.registerType) {
        case RegisterType::Coil: {
            nmbs_bitfield bits{};
            rc = nmbs_read_coils(&impl_->nmbs, batch.startAddress, batch.quantity, bits);
            if (rc == NMBS_ERROR_NONE) {
                bitsOut.resize(batch.quantity);
                for (std::uint16_t i = 0; i < batch.quantity; ++i) {
                    bitsOut[i] = nmbs_bitfield_read(bits, i);
                }
            }
            break;
        }
        case RegisterType::DiscreteInput: {
            nmbs_bitfield bits{};
            rc = nmbs_read_discrete_inputs(&impl_->nmbs, batch.startAddress, batch.quantity, bits);
            if (rc == NMBS_ERROR_NONE) {
                bitsOut.resize(batch.quantity);
                for (std::uint16_t i = 0; i < batch.quantity; ++i) {
                    bitsOut[i] = nmbs_bitfield_read(bits, i);
                }
            }
            break;
        }
        case RegisterType::HoldingRegister: {
            registersOut.resize(batch.quantity);
            rc = nmbs_read_holding_registers(&impl_->nmbs, batch.startAddress, batch.quantity,
                                              registersOut.data());
            break;
        }
        case RegisterType::InputRegister: {
            registersOut.resize(batch.quantity);
            rc = nmbs_read_input_registers(&impl_->nmbs, batch.startAddress, batch.quantity,
                                            registersOut.data());
            break;
        }
    }

    RequestResult result = translateModbusError(rc);
    if (!result.ok() && result.outcome != RequestOutcome::ModbusException) {
        close();
    }
    return result;
}

RequestResult ModbusRtuBus::writeRegisters(std::uint8_t unitId, std::uint16_t address,
                                            const std::vector<std::uint16_t>& values) {
    RequestResult openResult = ensureOpen();
    if (!openResult.ok()) return openResult;
    nmbs_set_destination_rtu_address(&impl_->nmbs, unitId);

    const nmbs_error rc = nmbs_write_multiple_registers(
        &impl_->nmbs, address, static_cast<std::uint16_t>(values.size()), values.data());

    RequestResult result = translateModbusError(rc);
    if (!result.ok() && result.outcome != RequestOutcome::ModbusException) {
        close();
    }
    return result;
}

RequestResult ModbusRtuBus::writeCoils(std::uint8_t unitId, std::uint16_t address,
                                        const std::vector<bool>& values) {
    RequestResult openResult = ensureOpen();
    if (!openResult.ok()) return openResult;
    nmbs_set_destination_rtu_address(&impl_->nmbs, unitId);

    nmbs_bitfield bits{};
    nmbs_bitfield_reset(bits);
    for (std::size_t i = 0; i < values.size(); ++i) {
        nmbs_bitfield_write(bits, i, values[i] ? 1 : 0);
    }

    const nmbs_error rc = nmbs_write_multiple_coils(&impl_->nmbs, address,
                                                      static_cast<std::uint16_t>(values.size()), bits);

    RequestResult result = translateModbusError(rc);
    if (!result.ok() && result.outcome != RequestOutcome::ModbusException) {
        close();
    }
    return result;
}

}  // namespace softplc::io
