#include "softplc/io/modbus_rtu_driver.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "softplc/core/native_program.hpp"
#include "softplc/core/scan_engine.hpp"
#include "softplc/tags/tag_store.hpp"
#include "support/mock_modbus_rtu_server.hpp"

using namespace softplc;
using namespace softplc::io;

namespace {

ModbusRtuBusConfig makeBusConfig(const std::string& devicePath, std::uint8_t unitId,
                                  const std::vector<ModbusPointMapping>& points) {
    ModbusRtuBusConfig config;
    config.name = "bus";
    config.devicePath = devicePath;
    config.serial = net::SerialConfig{.baudRate = 9600,
                                       .dataBits = 8,
                                       .parity = net::SerialParity::None,
                                       .stopBits = 1};
    config.requestTimeout = std::chrono::milliseconds(300);
    config.pollInterval = std::chrono::milliseconds(10);
    config.reconnectBackoff = std::chrono::milliseconds(50);

    ModbusRtuDeviceConfig device;
    device.name = "dev";
    device.unitId = unitId;
    device.points = points;
    config.devices.push_back(device);
    return config;
}

}  // namespace

TEST(ModbusRtuIoDriverTest, ReadInputsReflectsDeviceValueAfterPoll) {
    test::MockModbusRtuServer server(5);
    server.start();
    server.setHoldingRegister(0, 4242);

    tags::TagStore tags;
    const tags::Address addr{tags::MemoryArea::Input, 0, tags::Address::kNoBit};
    const auto tagId = tags.declare("InReg", tags::TypeId::Int, std::int16_t{0}, addr);

    ModbusPointMapping point;
    point.iecAddress = addr;
    point.type = tags::TypeId::Int;
    point.registerType = RegisterType::HoldingRegister;
    point.registerAddress = 0;

    ModbusRtuIoDriver driver({makeBusConfig(server.slavePath(), server.unitAddress(), {point})});
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

TEST(ModbusRtuIoDriverTest, WriteOutputsPushesValueToDevice) {
    test::MockModbusRtuServer server(5);
    server.start();

    tags::TagStore tags;
    const tags::Address addr{tags::MemoryArea::Output, 0, tags::Address::kNoBit};
    const auto tagId = tags.declare("OutReg", tags::TypeId::Int, std::int16_t{0}, addr);
    tags.write(tagId, std::int16_t{777});

    ModbusPointMapping point;
    point.iecAddress = addr;
    point.type = tags::TypeId::Int;
    point.registerType = RegisterType::HoldingRegister;
    point.registerAddress = 5;

    ModbusRtuIoDriver driver({makeBusConfig(server.slavePath(), server.unitAddress(), {point})});
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

TEST(ModbusRtuIoDriverTest, WorksAsDropInIoDriverViaScanEngine) {
    test::MockModbusRtuServer server(5);
    server.start();
    server.setHoldingRegister(0, 555);

    tags::TagStore tags;
    const tags::Address addr{tags::MemoryArea::Input, 0, tags::Address::kNoBit};
    const auto tagId = tags.declare("InReg", tags::TypeId::Int, std::int16_t{0}, addr);

    ModbusPointMapping point;
    point.iecAddress = addr;
    point.type = tags::TypeId::Int;
    point.registerType = RegisterType::HoldingRegister;
    point.registerAddress = 0;

    auto driver = std::make_shared<ModbusRtuIoDriver>(
        std::vector<ModbusRtuBusConfig>{makeBusConfig(server.slavePath(), server.unitAddress(), {point})});
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

TEST(ModbusRtuIoDriverTest, DeviceOfflineFreezesLastValueAndFlipsHealthTag) {
    test::MockModbusRtuServer server(5);
    server.start();
    server.setHoldingRegister(0, 111);

    tags::TagStore tags;
    const tags::Address inAddr{tags::MemoryArea::Input, 0, tags::Address::kNoBit};
    const auto inTagId = tags.declare("InReg", tags::TypeId::Int, std::int16_t{0}, inAddr);
    const tags::Address healthAddr{tags::MemoryArea::Memory, 0, tags::Address::kNoBit};
    const auto healthTagId = tags.declare("DevHealth", tags::TypeId::Bool, false, healthAddr);

    ModbusPointMapping point;
    point.iecAddress = inAddr;
    point.type = tags::TypeId::Int;
    point.registerType = RegisterType::HoldingRegister;
    point.registerAddress = 0;

    ModbusRtuBusConfig busConfig = makeBusConfig(server.slavePath(), server.unitAddress(), {point});
    busConfig.devices[0].healthTagAddress = healthAddr;
    ModbusRtuIoDriver driver({busConfig});
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

TEST(ModbusRtuIoDriverTest, DiagnosticsReportsBusAndDeviceCounts) {
    test::MockModbusRtuServer server(5);
    server.start();

    ModbusPointMapping point;
    point.iecAddress = tags::Address{tags::MemoryArea::Input, 0, tags::Address::kNoBit};
    point.type = tags::TypeId::Int;
    point.registerType = RegisterType::HoldingRegister;
    point.registerAddress = 0;

    ModbusRtuIoDriver driver({makeBusConfig(server.slavePath(), server.unitAddress(), {point})});
    EXPECT_EQ(driver.busCount(), 1u);
    EXPECT_EQ(driver.deviceCount(0), 1u);
    EXPECT_THROW((void)driver.deviceCount(1), std::out_of_range);
    EXPECT_THROW(driver.diagnostics(0, 1), std::out_of_range);
}
