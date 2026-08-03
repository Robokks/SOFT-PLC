#include "softplc/io/modbus_client.hpp"

#include <chrono>
#include <cstring>
#include <span>
#include <utility>

#include "nanomodbus.h"

namespace softplc::io {

namespace {

std::int32_t transportRead(std::uint8_t* buf, std::uint16_t count, std::int32_t byteTimeoutMs, void* arg) {
    auto* socket = static_cast<net::TcpSocket*>(arg);
    const auto timeout = std::chrono::milliseconds(byteTimeoutMs < 0 ? 60000 : byteTimeoutMs);
    return socket->recvSome(std::span<std::byte>(reinterpret_cast<std::byte*>(buf), count), timeout);
}

std::int32_t transportWrite(const std::uint8_t* buf, std::uint16_t count, std::int32_t byteTimeoutMs,
                             void* arg) {
    auto* socket = static_cast<net::TcpSocket*>(arg);
    const auto timeout = std::chrono::milliseconds(byteTimeoutMs < 0 ? 60000 : byteTimeoutMs);
    return socket->sendSome(std::span<const std::byte>(reinterpret_cast<const std::byte*>(buf), count),
                             timeout);
}

RequestResult translateError(nmbs_error rc) {
    RequestResult result;
    if (rc == NMBS_ERROR_NONE) {
        return result;  // outcome defaults to Success
    }
    if (nmbs_error_is_exception(rc)) {
        result.outcome = RequestOutcome::ModbusException;
        result.modbusExceptionCode = static_cast<std::uint8_t>(rc);
        result.detail = nmbs_strerror(rc);
        return result;
    }

    result.detail = nmbs_strerror(rc);
    switch (rc) {
        case NMBS_ERROR_TIMEOUT:
            result.outcome = RequestOutcome::Timeout;
            break;
        case NMBS_ERROR_TRANSPORT:
        case NMBS_ERROR_INVALID_TCP_MBAP:
        case NMBS_ERROR_INVALID_UNIT_ID:
        case NMBS_ERROR_CRC:
            result.outcome = RequestOutcome::ConnectionLost;
            break;
        default:
            result.outcome = RequestOutcome::ProtocolError;
            break;
    }
    return result;
}

}  // namespace

struct ModbusClient::Impl {
    nmbs_t nmbs{};
    bool created = false;
};

ModbusClient::ModbusClient(ModbusDeviceConfig config)
    : config_(std::move(config)), impl_(std::make_unique<Impl>()) {}

ModbusClient::~ModbusClient() = default;

bool ModbusClient::isConnected() const { return socket_.isOpen() && impl_->created; }

void ModbusClient::disconnect() {
    socket_.close();
    impl_->created = false;
}

RequestResult ModbusClient::ensureConnected() {
    if (isConnected()) {
        return RequestResult{};
    }
    disconnect();

    const net::SocketError socketError =
        socket_.connect(config_.host, config_.port, config_.connectTimeout);
    if (socketError != net::SocketError::None) {
        RequestResult result;
        result.outcome = RequestOutcome::ConnectFailed;
        result.detail = net::toString(socketError);
        return result;
    }

    nmbs_platform_conf conf;
    nmbs_platform_conf_create(&conf);
    conf.transport = NMBS_TRANSPORT_TCP;
    conf.read = transportRead;
    conf.write = transportWrite;
    conf.arg = &socket_;

    const nmbs_error rc = nmbs_client_create(&impl_->nmbs, &conf);
    if (rc != NMBS_ERROR_NONE) {
        socket_.close();
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

RequestResult ModbusClient::readBatch(const ReadBatch& batch, std::vector<std::uint16_t>& registersOut,
                                       std::vector<bool>& bitsOut) {
    RequestResult connectResult = ensureConnected();
    if (!connectResult.ok()) return connectResult;

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

    RequestResult result = translateError(rc);
    if (!result.ok() && result.outcome != RequestOutcome::ModbusException) {
        disconnect();
    }
    return result;
}

RequestResult ModbusClient::writeRegisters(std::uint16_t address,
                                            const std::vector<std::uint16_t>& values) {
    RequestResult connectResult = ensureConnected();
    if (!connectResult.ok()) return connectResult;

    const nmbs_error rc = nmbs_write_multiple_registers(
        &impl_->nmbs, address, static_cast<std::uint16_t>(values.size()), values.data());

    RequestResult result = translateError(rc);
    if (!result.ok() && result.outcome != RequestOutcome::ModbusException) {
        disconnect();
    }
    return result;
}

RequestResult ModbusClient::writeCoils(std::uint16_t address, const std::vector<bool>& values) {
    RequestResult connectResult = ensureConnected();
    if (!connectResult.ok()) return connectResult;

    nmbs_bitfield bits{};
    nmbs_bitfield_reset(bits);
    for (std::size_t i = 0; i < values.size(); ++i) {
        nmbs_bitfield_write(bits, i, values[i] ? 1 : 0);
    }

    const nmbs_error rc = nmbs_write_multiple_coils(&impl_->nmbs, address,
                                                      static_cast<std::uint16_t>(values.size()), bits);

    RequestResult result = translateError(rc);
    if (!result.ok() && result.outcome != RequestOutcome::ModbusException) {
        disconnect();
    }
    return result;
}

}  // namespace softplc::io
