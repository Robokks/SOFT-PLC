#include "softplc/io/modbus_client.hpp"

#include <gtest/gtest.h>

#include <chrono>

#include "softplc/net/tcp_listener.hpp"
#include "support/mock_modbus_server.hpp"

using namespace softplc;
using namespace softplc::io;

namespace {

ModbusDeviceConfig makeConfig(std::uint16_t port) {
    ModbusDeviceConfig config;
    config.name = "test-device";
    config.host = "127.0.0.1";
    config.port = port;
    config.connectTimeout = std::chrono::milliseconds(500);
    config.requestTimeout = std::chrono::milliseconds(300);
    return config;
}

}  // namespace

TEST(ModbusClientTest, ReadHoldingRegistersRoundTrip) {
    test::MockModbusServer server;
    server.start();
    server.setHoldingRegister(10, 1234);
    server.setHoldingRegister(11, 5678);

    ModbusClient client(makeConfig(server.port()));
    ReadBatch batch{RegisterType::HoldingRegister, 10, 2, {}};
    std::vector<std::uint16_t> registers;
    std::vector<bool> bits;
    const auto result = client.readBatch(batch, registers, bits);

    ASSERT_TRUE(result.ok());
    ASSERT_EQ(registers.size(), 2u);
    EXPECT_EQ(registers[0], 1234);
    EXPECT_EQ(registers[1], 5678);
}

TEST(ModbusClientTest, ReadInputRegistersRoundTrip) {
    test::MockModbusServer server;
    server.start();
    server.setInputRegister(0, 42);

    ModbusClient client(makeConfig(server.port()));
    ReadBatch batch{RegisterType::InputRegister, 0, 1, {}};
    std::vector<std::uint16_t> registers;
    std::vector<bool> bits;
    ASSERT_TRUE(client.readBatch(batch, registers, bits).ok());
    ASSERT_EQ(registers.size(), 1u);
    EXPECT_EQ(registers[0], 42);
}

TEST(ModbusClientTest, ReadCoilsRoundTrip) {
    test::MockModbusServer server;
    server.start();
    server.setCoil(0, true);
    server.setCoil(1, false);
    server.setCoil(2, true);

    ModbusClient client(makeConfig(server.port()));
    ReadBatch batch{RegisterType::Coil, 0, 3, {}};
    std::vector<std::uint16_t> registers;
    std::vector<bool> bits;
    ASSERT_TRUE(client.readBatch(batch, registers, bits).ok());
    ASSERT_EQ(bits.size(), 3u);
    EXPECT_TRUE(bits[0]);
    EXPECT_FALSE(bits[1]);
    EXPECT_TRUE(bits[2]);
}

TEST(ModbusClientTest, ReadDiscreteInputsRoundTrip) {
    test::MockModbusServer server;
    server.start();
    server.setDiscreteInput(0, true);

    ModbusClient client(makeConfig(server.port()));
    ReadBatch batch{RegisterType::DiscreteInput, 0, 1, {}};
    std::vector<std::uint16_t> registers;
    std::vector<bool> bits;
    ASSERT_TRUE(client.readBatch(batch, registers, bits).ok());
    ASSERT_EQ(bits.size(), 1u);
    EXPECT_TRUE(bits[0]);
}

TEST(ModbusClientTest, WriteRegistersRoundTrip) {
    test::MockModbusServer server;
    server.start();

    ModbusClient client(makeConfig(server.port()));
    const auto result = client.writeRegisters(0, {111, 222});
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(server.getHoldingRegister(0), 111);
    EXPECT_EQ(server.getHoldingRegister(1), 222);
}

TEST(ModbusClientTest, WriteCoilsRoundTrip) {
    test::MockModbusServer server;
    server.start();

    ModbusClient client(makeConfig(server.port()));
    const auto result = client.writeCoils(0, {true, false, true});
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(server.getCoil(0));
    EXPECT_FALSE(server.getCoil(1));
    EXPECT_TRUE(server.getCoil(2));
}

TEST(ModbusClientTest, ConnectFailedOnUnreachableDevice) {
    net::TcpListener probe;
    ASSERT_TRUE(probe.listen(0));
    const std::uint16_t freePort = probe.boundPort();
    probe.close();  // nothing listens on this port anymore

    ModbusDeviceConfig config = makeConfig(freePort);
    config.connectTimeout = std::chrono::milliseconds(300);
    ModbusClient client(config);

    ReadBatch batch{RegisterType::HoldingRegister, 0, 1, {}};
    std::vector<std::uint16_t> registers;
    std::vector<bool> bits;
    const auto result = client.readBatch(batch, registers, bits);
    EXPECT_EQ(result.outcome, RequestOutcome::ConnectFailed);
}

TEST(ModbusClientTest, TimeoutWhenResponseDropped) {
    test::MockModbusServer server;
    server.start();
    server.setDropNextNResponses(1);

    ModbusDeviceConfig config = makeConfig(server.port());
    config.requestTimeout = std::chrono::milliseconds(150);
    ModbusClient client(config);

    ReadBatch batch{RegisterType::HoldingRegister, 0, 1, {}};
    std::vector<std::uint16_t> registers;
    std::vector<bool> bits;
    const auto result = client.readBatch(batch, registers, bits);
    EXPECT_EQ(result.outcome, RequestOutcome::Timeout);
}

TEST(ModbusClientTest, ReconnectsAfterConnectionClosedMidSession) {
    test::MockModbusServer server;
    server.start();
    server.setHoldingRegister(0, 99);

    ModbusClient client(makeConfig(server.port()));
    ReadBatch batch{RegisterType::HoldingRegister, 0, 1, {}};
    std::vector<std::uint16_t> registers;
    std::vector<bool> bits;
    ASSERT_TRUE(client.readBatch(batch, registers, bits).ok());
    ASSERT_TRUE(client.isConnected());

    server.closeConnectionOnNextRequest();
    const auto failedResult = client.readBatch(batch, registers, bits);
    EXPECT_FALSE(failedResult.ok());
    EXPECT_FALSE(client.isConnected());

    // The next call should transparently reconnect and succeed.
    registers.clear();
    const auto retryResult = client.readBatch(batch, registers, bits);
    ASSERT_TRUE(retryResult.ok());
    ASSERT_EQ(registers.size(), 1u);
    EXPECT_EQ(registers[0], 99);
}

TEST(ModbusClientTest, ModbusExceptionSurfacedWithoutDisconnecting) {
    test::MockModbusServer server;
    server.start();

    ModbusClient client(makeConfig(server.port()));
    // address 998 + quantity 5 is valid client-side (nanoMODBUS itself wouldn't
    // reject it) but exceeds the mock server's deliberately small address space, so
    // its onReadHoldingRegisters callback returns an illegal-data-address exception
    // -- a genuine round-tripped protocol response, not a client-side/transport
    // failure.
    ReadBatch batch{RegisterType::HoldingRegister, 998, 5, {}};
    std::vector<std::uint16_t> registers;
    std::vector<bool> bits;
    const auto result = client.readBatch(batch, registers, bits);

    ASSERT_EQ(result.outcome, RequestOutcome::ModbusException);
    EXPECT_TRUE(client.isConnected());
}
