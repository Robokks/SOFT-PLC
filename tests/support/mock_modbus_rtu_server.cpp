#include "support/mock_modbus_rtu_server.hpp"

#include <pty.h>
#include <unistd.h>

#include <cstring>
#include <span>

namespace softplc::test {

MockModbusRtuServer::MockModbusRtuServer(std::uint8_t unitAddress) : unitAddress_(unitAddress) {
    int slaveFd = -1;
    char nameBuf[256];
    if (openpty(&masterFd_, &slaveFd, nameBuf, nullptr, nullptr) != 0) {
        masterFd_ = -1;
        return;
    }
    slavePath_ = nameBuf;
    ::close(slaveFd);  // the client (ModbusRtuBus) reopens the slave by path
    masterPort_ = net::SerialPort::adopt(masterFd_);
}

MockModbusRtuServer::~MockModbusRtuServer() { stop(); }

void MockModbusRtuServer::start() {
    if (thread_.joinable()) return;
    thread_ = std::jthread([this](std::stop_token stopToken) { serveLoop(stopToken); });
}

void MockModbusRtuServer::stop() {
    if (!thread_.joinable()) return;
    thread_.request_stop();
    thread_.join();
}

void MockModbusRtuServer::setCoil(std::uint16_t address, bool value) {
    std::lock_guard<std::mutex> lock(registersMutex_);
    coils_[address] = value;
}

bool MockModbusRtuServer::getCoil(std::uint16_t address) const {
    std::lock_guard<std::mutex> lock(registersMutex_);
    return coils_[address];
}

void MockModbusRtuServer::setDiscreteInput(std::uint16_t address, bool value) {
    std::lock_guard<std::mutex> lock(registersMutex_);
    discreteInputs_[address] = value;
}

void MockModbusRtuServer::setHoldingRegister(std::uint16_t address, std::uint16_t value) {
    std::lock_guard<std::mutex> lock(registersMutex_);
    holdingRegisters_[address] = value;
}

std::uint16_t MockModbusRtuServer::getHoldingRegister(std::uint16_t address) const {
    std::lock_guard<std::mutex> lock(registersMutex_);
    return holdingRegisters_[address];
}

void MockModbusRtuServer::setInputRegister(std::uint16_t address, std::uint16_t value) {
    std::lock_guard<std::mutex> lock(registersMutex_);
    inputRegisters_[address] = value;
}

void MockModbusRtuServer::serveLoop(std::stop_token stopToken) {
    if (masterFd_ < 0) return;

    nmbs_platform_conf conf;
    nmbs_platform_conf_create(&conf);
    conf.transport = NMBS_TRANSPORT_RTU;
    conf.read = &MockModbusRtuServer::transportRead;
    conf.write = &MockModbusRtuServer::transportWrite;
    conf.arg = this;

    nmbs_callbacks callbacks;
    nmbs_callbacks_create(&callbacks);
    callbacks.read_coils = &MockModbusRtuServer::onReadCoils;
    callbacks.read_discrete_inputs = &MockModbusRtuServer::onReadDiscreteInputs;
    callbacks.read_holding_registers = &MockModbusRtuServer::onReadHoldingRegisters;
    callbacks.read_input_registers = &MockModbusRtuServer::onReadInputRegisters;
    callbacks.write_multiple_coils = &MockModbusRtuServer::onWriteMultipleCoils;
    callbacks.write_multiple_registers = &MockModbusRtuServer::onWriteMultipleRegisters;
    callbacks.arg = this;

    nmbs_t nmbs{};
    if (nmbs_server_create(&nmbs, unitAddress_, &conf, &callbacks) != NMBS_ERROR_NONE) {
        return;
    }
    nmbs_set_read_timeout(&nmbs, 50);  // short poll so the stop_token is checked regularly

    while (!stopToken.stop_requested()) {
        nmbs_server_poll(&nmbs);
    }
}

std::int32_t MockModbusRtuServer::transportRead(std::uint8_t* buf, std::uint16_t count,
                                                 std::int32_t byteTimeoutMs, void* arg) {
    auto* server = static_cast<MockModbusRtuServer*>(arg);
    const auto timeout = std::chrono::milliseconds(byteTimeoutMs < 0 ? 60000 : byteTimeoutMs);
    return server->masterPort_.recvSome(std::span<std::byte>(reinterpret_cast<std::byte*>(buf), count),
                                         timeout);
}

std::int32_t MockModbusRtuServer::transportWrite(const std::uint8_t* buf, std::uint16_t count,
                                                  std::int32_t byteTimeoutMs, void* arg) {
    auto* server = static_cast<MockModbusRtuServer*>(arg);
    const auto timeout = std::chrono::milliseconds(byteTimeoutMs < 0 ? 60000 : byteTimeoutMs);
    return server->masterPort_.sendSome(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(buf), count), timeout);
}

nmbs_error MockModbusRtuServer::onReadCoils(std::uint16_t address, std::uint16_t quantity,
                                             nmbs_bitfield coilsOut, std::uint8_t /*unitId*/, void* arg) {
    auto* server = static_cast<MockModbusRtuServer*>(arg);
    if (static_cast<std::uint32_t>(address) + quantity > kAddressSpace) {
        return NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS;
    }
    std::lock_guard<std::mutex> lock(server->registersMutex_);
    for (std::uint16_t i = 0; i < quantity; ++i) {
        if (server->coils_[static_cast<std::uint32_t>(address) + i]) {
            nmbs_bitfield_set(coilsOut, i);
        } else {
            nmbs_bitfield_unset(coilsOut, i);
        }
    }
    return NMBS_ERROR_NONE;
}

nmbs_error MockModbusRtuServer::onReadDiscreteInputs(std::uint16_t address, std::uint16_t quantity,
                                                       nmbs_bitfield inputsOut, std::uint8_t /*unitId*/,
                                                       void* arg) {
    auto* server = static_cast<MockModbusRtuServer*>(arg);
    if (static_cast<std::uint32_t>(address) + quantity > kAddressSpace) {
        return NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS;
    }
    std::lock_guard<std::mutex> lock(server->registersMutex_);
    for (std::uint16_t i = 0; i < quantity; ++i) {
        if (server->discreteInputs_[static_cast<std::uint32_t>(address) + i]) {
            nmbs_bitfield_set(inputsOut, i);
        } else {
            nmbs_bitfield_unset(inputsOut, i);
        }
    }
    return NMBS_ERROR_NONE;
}

nmbs_error MockModbusRtuServer::onReadHoldingRegisters(std::uint16_t address, std::uint16_t quantity,
                                                         std::uint16_t* registersOut,
                                                         std::uint8_t /*unitId*/, void* arg) {
    auto* server = static_cast<MockModbusRtuServer*>(arg);
    if (static_cast<std::uint32_t>(address) + quantity > kAddressSpace) {
        return NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS;
    }
    std::lock_guard<std::mutex> lock(server->registersMutex_);
    for (std::uint16_t i = 0; i < quantity; ++i) {
        registersOut[i] = server->holdingRegisters_[static_cast<std::uint32_t>(address) + i];
    }
    return NMBS_ERROR_NONE;
}

nmbs_error MockModbusRtuServer::onReadInputRegisters(std::uint16_t address, std::uint16_t quantity,
                                                       std::uint16_t* registersOut, std::uint8_t /*unitId*/,
                                                       void* arg) {
    auto* server = static_cast<MockModbusRtuServer*>(arg);
    if (static_cast<std::uint32_t>(address) + quantity > kAddressSpace) {
        return NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS;
    }
    std::lock_guard<std::mutex> lock(server->registersMutex_);
    for (std::uint16_t i = 0; i < quantity; ++i) {
        registersOut[i] = server->inputRegisters_[static_cast<std::uint32_t>(address) + i];
    }
    return NMBS_ERROR_NONE;
}

nmbs_error MockModbusRtuServer::onWriteMultipleCoils(std::uint16_t address, std::uint16_t quantity,
                                                       const nmbs_bitfield coils, std::uint8_t /*unitId*/,
                                                       void* arg) {
    auto* server = static_cast<MockModbusRtuServer*>(arg);
    if (static_cast<std::uint32_t>(address) + quantity > kAddressSpace) {
        return NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS;
    }
    std::lock_guard<std::mutex> lock(server->registersMutex_);
    for (std::uint16_t i = 0; i < quantity; ++i) {
        server->coils_[static_cast<std::uint32_t>(address) + i] = nmbs_bitfield_read(coils, i);
    }
    return NMBS_ERROR_NONE;
}

nmbs_error MockModbusRtuServer::onWriteMultipleRegisters(std::uint16_t address, std::uint16_t quantity,
                                                           const std::uint16_t* registers,
                                                           std::uint8_t /*unitId*/, void* arg) {
    auto* server = static_cast<MockModbusRtuServer*>(arg);
    if (static_cast<std::uint32_t>(address) + quantity > kAddressSpace) {
        return NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS;
    }
    std::lock_guard<std::mutex> lock(server->registersMutex_);
    for (std::uint16_t i = 0; i < quantity; ++i) {
        server->holdingRegisters_[static_cast<std::uint32_t>(address) + i] = registers[i];
    }
    return NMBS_ERROR_NONE;
}

}  // namespace softplc::test
