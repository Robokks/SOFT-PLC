#include "support/mock_modbus_server.hpp"

#include <span>

namespace softplc::test {

namespace {

// Per-connection context passed as nmbs_platform_conf::arg to the transport
// callbacks. The register-access callbacks only need `this` (MockModbusServer*),
// passed separately as nmbs_callbacks::arg.
struct ConnectionContext {
    MockModbusServer* server;
    net::TcpSocket* socket;
};

}  // namespace

MockModbusServer::MockModbusServer() = default;
MockModbusServer::~MockModbusServer() { stop(); }

void MockModbusServer::start() {
    if (acceptThread_.joinable()) return;
    listener_.listen(0);
    acceptThread_ = std::jthread([this](std::stop_token stopToken) { acceptLoop(stopToken); });
}

void MockModbusServer::stop() {
    if (!acceptThread_.joinable()) return;
    acceptThread_.request_stop();
    acceptThread_.join();
    listener_.close();
}

void MockModbusServer::setCoil(std::uint16_t address, bool value) {
    std::lock_guard<std::mutex> lock(registersMutex_);
    coils_[address] = value;
}

bool MockModbusServer::getCoil(std::uint16_t address) const {
    std::lock_guard<std::mutex> lock(registersMutex_);
    return coils_[address];
}

void MockModbusServer::setDiscreteInput(std::uint16_t address, bool value) {
    std::lock_guard<std::mutex> lock(registersMutex_);
    discreteInputs_[address] = value;
}

void MockModbusServer::setHoldingRegister(std::uint16_t address, std::uint16_t value) {
    std::lock_guard<std::mutex> lock(registersMutex_);
    holdingRegisters_[address] = value;
}

std::uint16_t MockModbusServer::getHoldingRegister(std::uint16_t address) const {
    std::lock_guard<std::mutex> lock(registersMutex_);
    return holdingRegisters_[address];
}

void MockModbusServer::setInputRegister(std::uint16_t address, std::uint16_t value) {
    std::lock_guard<std::mutex> lock(registersMutex_);
    inputRegisters_[address] = value;
}

void MockModbusServer::setResponseDelay(std::chrono::milliseconds delay) {
    responseDelayMs_.store(delay.count());
}

void MockModbusServer::setDropNextNResponses(int n) { dropNextN_.store(n); }

void MockModbusServer::closeConnectionOnNextRequest() { closeOnNextRequest_.store(true); }

void MockModbusServer::rejectConnections(bool reject) { rejectConnections_.store(reject); }

bool MockModbusServer::takeDropSignal() {
    int expected = dropNextN_.load();
    while (expected > 0) {
        if (dropNextN_.compare_exchange_weak(expected, expected - 1)) return true;
    }
    return false;
}

bool MockModbusServer::takeCloseSignal() {
    bool expected = true;
    return closeOnNextRequest_.compare_exchange_strong(expected, false);
}

std::chrono::milliseconds MockModbusServer::responseDelay() const {
    return std::chrono::milliseconds(responseDelayMs_.load());
}

void MockModbusServer::acceptLoop(std::stop_token stopToken) {
    while (!stopToken.stop_requested()) {
        std::optional<net::TcpSocket> accepted = listener_.accept(std::chrono::milliseconds(100));
        if (!accepted) continue;
        if (rejectConnections_.load()) {
            continue;  // accepted socket destructs here, closing the connection immediately
        }
        serveConnection(std::move(*accepted), stopToken);
    }
}

void MockModbusServer::serveConnection(net::TcpSocket socket, std::stop_token stopToken) {
    ConnectionContext ctx{this, &socket};

    nmbs_platform_conf conf;
    nmbs_platform_conf_create(&conf);
    conf.transport = NMBS_TRANSPORT_TCP;
    conf.read = &MockModbusServer::transportRead;
    conf.write = &MockModbusServer::transportWrite;
    conf.arg = &ctx;

    nmbs_callbacks callbacks;
    nmbs_callbacks_create(&callbacks);
    callbacks.read_coils = &MockModbusServer::onReadCoils;
    callbacks.read_discrete_inputs = &MockModbusServer::onReadDiscreteInputs;
    callbacks.read_holding_registers = &MockModbusServer::onReadHoldingRegisters;
    callbacks.read_input_registers = &MockModbusServer::onReadInputRegisters;
    callbacks.write_single_coil = &MockModbusServer::onWriteSingleCoil;
    callbacks.write_single_register = &MockModbusServer::onWriteSingleRegister;
    callbacks.write_multiple_coils = &MockModbusServer::onWriteMultipleCoils;
    callbacks.write_multiple_registers = &MockModbusServer::onWriteMultipleRegisters;
    callbacks.arg = this;

    nmbs_t nmbs{};
    if (nmbs_server_create(&nmbs, 0, &conf, &callbacks) != NMBS_ERROR_NONE) {
        return;
    }
    nmbs_set_read_timeout(&nmbs, 100);  // short poll so the stop_token is checked regularly

    while (!stopToken.stop_requested() && socket.isOpen()) {
        const nmbs_error rc = nmbs_server_poll(&nmbs);
        if (rc == NMBS_ERROR_TRANSPORT) break;  // connection closed/broken: back to accept()
    }
}

std::int32_t MockModbusServer::transportRead(std::uint8_t* buf, std::uint16_t count,
                                              std::int32_t byteTimeoutMs, void* arg) {
    auto* ctx = static_cast<ConnectionContext*>(arg);
    const auto timeout = std::chrono::milliseconds(byteTimeoutMs < 0 ? 60000 : byteTimeoutMs);
    return ctx->socket->recvSome(std::span<std::byte>(reinterpret_cast<std::byte*>(buf), count), timeout);
}

std::int32_t MockModbusServer::transportWrite(const std::uint8_t* buf, std::uint16_t count,
                                               std::int32_t byteTimeoutMs, void* arg) {
    auto* ctx = static_cast<ConnectionContext*>(arg);
    MockModbusServer* server = ctx->server;

    if (server->takeCloseSignal()) {
        ctx->socket->close();
        return -1;
    }

    const auto delay = server->responseDelay();
    if (delay.count() > 0) {
        std::this_thread::sleep_for(delay);
    }

    if (server->takeDropSignal()) {
        return static_cast<std::int32_t>(count);  // pretend success without actually sending
    }

    const auto timeout = std::chrono::milliseconds(byteTimeoutMs < 0 ? 60000 : byteTimeoutMs);
    return ctx->socket->sendSome(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(buf), count), timeout);
}

nmbs_error MockModbusServer::onReadCoils(std::uint16_t address, std::uint16_t quantity,
                                          nmbs_bitfield coilsOut, std::uint8_t /*unitId*/, void* arg) {
    auto* server = static_cast<MockModbusServer*>(arg);
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

nmbs_error MockModbusServer::onReadDiscreteInputs(std::uint16_t address, std::uint16_t quantity,
                                                   nmbs_bitfield inputsOut, std::uint8_t /*unitId*/,
                                                   void* arg) {
    auto* server = static_cast<MockModbusServer*>(arg);
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

nmbs_error MockModbusServer::onReadHoldingRegisters(std::uint16_t address, std::uint16_t quantity,
                                                      std::uint16_t* registersOut, std::uint8_t /*unitId*/,
                                                      void* arg) {
    auto* server = static_cast<MockModbusServer*>(arg);
    if (static_cast<std::uint32_t>(address) + quantity > kAddressSpace) {
        return NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS;
    }
    std::lock_guard<std::mutex> lock(server->registersMutex_);
    for (std::uint16_t i = 0; i < quantity; ++i) {
        registersOut[i] = server->holdingRegisters_[static_cast<std::uint32_t>(address) + i];
    }
    return NMBS_ERROR_NONE;
}

nmbs_error MockModbusServer::onReadInputRegisters(std::uint16_t address, std::uint16_t quantity,
                                                    std::uint16_t* registersOut, std::uint8_t /*unitId*/,
                                                    void* arg) {
    auto* server = static_cast<MockModbusServer*>(arg);
    if (static_cast<std::uint32_t>(address) + quantity > kAddressSpace) {
        return NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS;
    }
    std::lock_guard<std::mutex> lock(server->registersMutex_);
    for (std::uint16_t i = 0; i < quantity; ++i) {
        registersOut[i] = server->inputRegisters_[static_cast<std::uint32_t>(address) + i];
    }
    return NMBS_ERROR_NONE;
}

nmbs_error MockModbusServer::onWriteSingleCoil(std::uint16_t address, bool value, std::uint8_t /*unitId*/,
                                                void* arg) {
    auto* server = static_cast<MockModbusServer*>(arg);
    std::lock_guard<std::mutex> lock(server->registersMutex_);
    server->coils_[address] = value;
    return NMBS_ERROR_NONE;
}

nmbs_error MockModbusServer::onWriteSingleRegister(std::uint16_t address, std::uint16_t value,
                                                     std::uint8_t /*unitId*/, void* arg) {
    auto* server = static_cast<MockModbusServer*>(arg);
    std::lock_guard<std::mutex> lock(server->registersMutex_);
    server->holdingRegisters_[address] = value;
    return NMBS_ERROR_NONE;
}

nmbs_error MockModbusServer::onWriteMultipleCoils(std::uint16_t address, std::uint16_t quantity,
                                                    const nmbs_bitfield coils, std::uint8_t /*unitId*/,
                                                    void* arg) {
    auto* server = static_cast<MockModbusServer*>(arg);
    if (static_cast<std::uint32_t>(address) + quantity > kAddressSpace) {
        return NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS;
    }
    std::lock_guard<std::mutex> lock(server->registersMutex_);
    for (std::uint16_t i = 0; i < quantity; ++i) {
        server->coils_[static_cast<std::uint32_t>(address) + i] = nmbs_bitfield_read(coils, i);
    }
    return NMBS_ERROR_NONE;
}

nmbs_error MockModbusServer::onWriteMultipleRegisters(std::uint16_t address, std::uint16_t quantity,
                                                        const std::uint16_t* registers,
                                                        std::uint8_t /*unitId*/, void* arg) {
    auto* server = static_cast<MockModbusServer*>(arg);
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
