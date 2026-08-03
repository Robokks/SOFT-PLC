#include "softplc/io/modbus_tcp_driver.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "softplc/core/native_program.hpp"
#include "softplc/core/scan_engine.hpp"
#include "softplc/tags/tag_store.hpp"
#include "support/mock_modbus_server.hpp"

using namespace softplc;
using namespace softplc::io;

namespace {

ModbusDeviceConfig makeSinglePointConfig(std::uint16_t port, tags::TypeId type, RegisterType regType,
                                          std::uint16_t regAddr, tags::Address iecAddr) {
    ModbusDeviceConfig config;
    config.name = "dev";
    config.host = "127.0.0.1";
    config.port = port;
    config.pollInterval = std::chrono::milliseconds(10);
    config.connectTimeout = std::chrono::milliseconds(500);
    config.requestTimeout = std::chrono::milliseconds(300);
    config.reconnectBackoff = std::chrono::milliseconds(50);

    ModbusPointMapping point;
    point.iecAddress = iecAddr;
    point.type = type;
    point.registerType = regType;
    point.registerAddress = regAddr;
    config.points.push_back(point);
    return config;
}

}  // namespace

TEST(ModbusTcpIoDriverTest, ReadInputsReflectsDeviceValueAfterPoll) {
    test::MockModbusServer server;
    server.start();
    server.setHoldingRegister(0, 4242);

    tags::TagStore tags;
    const tags::Address addr{tags::MemoryArea::Input, 0, tags::Address::kNoBit};
    const auto tagId = tags.declare("InReg", tags::TypeId::Int, std::int16_t{0}, addr);

    ModbusDeviceConfig config =
        makeSinglePointConfig(server.port(), tags::TypeId::Int, RegisterType::HoldingRegister, 0, addr);
    ModbusTcpIoDriver driver({config});
    driver.start();

    bool sawValue = false;
    for (int i = 0; i < 100 && !sawValue; ++i) {
        driver.readInputs(tags);
        if (std::get<std::int16_t>(tags.read(tagId)) == 4242) sawValue = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    driver.stop();

    EXPECT_TRUE(sawValue);
}

TEST(ModbusTcpIoDriverTest, WriteOutputsPushesValueToDevice) {
    test::MockModbusServer server;
    server.start();

    tags::TagStore tags;
    const tags::Address addr{tags::MemoryArea::Output, 0, tags::Address::kNoBit};
    const auto tagId = tags.declare("OutReg", tags::TypeId::Int, std::int16_t{0}, addr);
    tags.write(tagId, std::int16_t{777});

    ModbusDeviceConfig config =
        makeSinglePointConfig(server.port(), tags::TypeId::Int, RegisterType::HoldingRegister, 5, addr);
    ModbusTcpIoDriver driver({config});
    driver.start();

    bool wrote = false;
    for (int i = 0; i < 100 && !wrote; ++i) {
        driver.writeOutputs(tags);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (server.getHoldingRegister(5) == 777) wrote = true;
    }
    driver.stop();

    EXPECT_TRUE(wrote);
}

TEST(ModbusTcpIoDriverTest, WorksAsDropInIoDriverViaScanEngine) {
    test::MockModbusServer server;
    server.start();
    server.setHoldingRegister(0, 555);

    tags::TagStore tags;
    const tags::Address addr{tags::MemoryArea::Input, 0, tags::Address::kNoBit};
    const auto tagId = tags.declare("InReg", tags::TypeId::Int, std::int16_t{0}, addr);

    ModbusDeviceConfig config =
        makeSinglePointConfig(server.port(), tags::TypeId::Int, RegisterType::HoldingRegister, 0, addr);
    auto driver = std::make_shared<ModbusTcpIoDriver>(std::vector<ModbusDeviceConfig>{config});
    driver->start();

    auto program = std::make_shared<core::NativeProgram>("noop", [](core::ScanContext&) {});
    core::ScanEngine engine(tags, *driver, program, std::chrono::milliseconds(10));

    bool sawValue = false;
    for (int i = 0; i < 100 && !sawValue; ++i) {
        engine.runOnce();
        if (std::get<std::int16_t>(tags.read(tagId)) == 555) sawValue = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    driver->stop();
    EXPECT_TRUE(sawValue);
}

TEST(ModbusTcpIoDriverTest, DeviceOfflineFreezesLastValueAndFlipsHealthTag) {
    test::MockModbusServer server;
    server.start();
    server.setHoldingRegister(0, 111);

    tags::TagStore tags;
    const tags::Address inAddr{tags::MemoryArea::Input, 0, tags::Address::kNoBit};
    const auto inTagId = tags.declare("InReg", tags::TypeId::Int, std::int16_t{0}, inAddr);
    const tags::Address healthAddr{tags::MemoryArea::Memory, 0, tags::Address::kNoBit};
    const auto healthTagId = tags.declare("DevHealth", tags::TypeId::Bool, false, healthAddr);

    ModbusDeviceConfig config =
        makeSinglePointConfig(server.port(), tags::TypeId::Int, RegisterType::HoldingRegister, 0, inAddr);
    config.healthTagAddress = healthAddr;
    ModbusTcpIoDriver driver({config});
    driver.start();

    bool connectedOnce = false;
    for (int i = 0; i < 100 && !connectedOnce; ++i) {
        driver.readInputs(tags);
        if (std::get<bool>(tags.read(healthTagId))) connectedOnce = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_TRUE(connectedOnce);
    EXPECT_EQ(std::get<std::int16_t>(tags.read(inTagId)), 111);

    server.stop();

    bool sawOffline = false;
    for (int i = 0; i < 100 && !sawOffline; ++i) {
        driver.readInputs(tags);
        if (!std::get<bool>(tags.read(healthTagId))) sawOffline = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    driver.stop();

    ASSERT_TRUE(sawOffline);
    // Freeze-last-good-value: the input tag keeps its last known-good reading rather
    // than being reset/zeroed while the device is unreachable.
    EXPECT_EQ(std::get<std::int16_t>(tags.read(inTagId)), 111);
}
