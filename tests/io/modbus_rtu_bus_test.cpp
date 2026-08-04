#include "softplc/io/modbus_rtu_bus.hpp"

#include <gtest/gtest.h>

#include <chrono>

#include "support/mock_modbus_rtu_server.hpp"

using namespace softplc;
using namespace softplc::io;

namespace {

ModbusRtuBusConfig makeBusConfig(const std::string& devicePath) {
    ModbusRtuBusConfig config;
    config.name = "bus";
    config.devicePath = devicePath;
    config.serial = net::SerialConfig{.baudRate = 9600,
                                       .dataBits = 8,
                                       .parity = net::SerialParity::None,
                                       .stopBits = 1};
    config.requestTimeout = std::chrono::milliseconds(300);
    return config;
}

}  // namespace

TEST(ModbusRtuBusTest, ReadHoldingRegistersRoundTripWhenUnitIdMatches) {
    test::MockModbusRtuServer server(5);
    server.start();
    server.setHoldingRegister(10, 1234);
    server.setHoldingRegister(11, 5678);

    ModbusRtuBus bus(makeBusConfig(server.slavePath()));
    ReadBatch batch{RegisterType::HoldingRegister, 10, 2, {}};
    std::vector<std::uint16_t> registers;
    std::vector<bool> bits;
    const auto result = bus.readBatch(server.unitAddress(), batch, registers, bits);

    ASSERT_TRUE(result.ok());
    ASSERT_EQ(registers.size(), 2u);
    EXPECT_EQ(registers[0], 1234);
    EXPECT_EQ(registers[1], 5678);
}

TEST(ModbusRtuBusTest, ReadCoilsRoundTrip) {
    test::MockModbusRtuServer server(5);
    server.start();
    server.setCoil(0, true);
    server.setCoil(1, false);
    server.setCoil(2, true);

    ModbusRtuBus bus(makeBusConfig(server.slavePath()));
    ReadBatch batch{RegisterType::Coil, 0, 3, {}};
    std::vector<std::uint16_t> registers;
    std::vector<bool> bits;
    ASSERT_TRUE(bus.readBatch(server.unitAddress(), batch, registers, bits).ok());
    ASSERT_EQ(bits.size(), 3u);
    EXPECT_TRUE(bits[0]);
    EXPECT_FALSE(bits[1]);
    EXPECT_TRUE(bits[2]);
}

TEST(ModbusRtuBusTest, WriteRegistersRoundTrip) {
    test::MockModbusRtuServer server(5);
    server.start();

    ModbusRtuBus bus(makeBusConfig(server.slavePath()));
    const auto result = bus.writeRegisters(server.unitAddress(), 0, {111, 222});
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(server.getHoldingRegister(0), 111);
    EXPECT_EQ(server.getHoldingRegister(1), 222);
}

TEST(ModbusRtuBusTest, WriteCoilsRoundTrip) {
    test::MockModbusRtuServer server(5);
    server.start();

    ModbusRtuBus bus(makeBusConfig(server.slavePath()));
    const auto result = bus.writeCoils(server.unitAddress(), 0, {true, false, true});
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(server.getCoil(0));
    EXPECT_FALSE(server.getCoil(1));
    EXPECT_TRUE(server.getCoil(2));
}

TEST(ModbusRtuBusTest, RequestToMismatchedUnitIdTimesOut) {
    test::MockModbusRtuServer server(5);
    server.start();
    server.setHoldingRegister(0, 42);

    ModbusRtuBusConfig config = makeBusConfig(server.slavePath());
    config.requestTimeout = std::chrono::milliseconds(150);
    ModbusRtuBus bus(config);

    ReadBatch batch{RegisterType::HoldingRegister, 0, 1, {}};
    std::vector<std::uint16_t> registers;
    std::vector<bool> bits;
    // The mock server only answers requests addressed to unitAddress()==5; a request
    // to unit 9 on the very same shared port gets no response at all -- this is the
    // concrete proof that ModbusRtuBus actually transmits the unitId it's given
    // (nmbs_set_destination_rtu_address()) rather than a fixed/ignored value, which
    // is the whole point of a bus shared by multiple slaves.
    const auto result = bus.readBatch(/*unitId=*/9, batch, registers, bits);
    EXPECT_EQ(result.outcome, RequestOutcome::Timeout);
}

TEST(ModbusRtuBusTest, ModbusExceptionSurfacedWithoutClosingPort) {
    test::MockModbusRtuServer server(5);
    server.start();

    ModbusRtuBus bus(makeBusConfig(server.slavePath()));
    // address 998 + quantity 5 exceeds the mock server's deliberately small address
    // space, so its onReadHoldingRegisters callback returns a genuine round-tripped
    // Modbus exception rather than a client-side/transport failure.
    ReadBatch batch{RegisterType::HoldingRegister, 998, 5, {}};
    std::vector<std::uint16_t> registers;
    std::vector<bool> bits;
    const auto result = bus.readBatch(server.unitAddress(), batch, registers, bits);

    ASSERT_EQ(result.outcome, RequestOutcome::ModbusException);
    EXPECT_TRUE(bus.isOpen());
}
