#include "softplc/io/modbus_mapping.hpp"

#include <gtest/gtest.h>

using namespace softplc;
using namespace softplc::io;

namespace {

ModbusPointMapping makeRegisterPoint(std::uint16_t addr, tags::TypeId type,
                                      RegisterType regType = RegisterType::HoldingRegister,
                                      tags::MemoryArea area = tags::MemoryArea::Memory) {
    ModbusPointMapping point;
    point.iecAddress = tags::Address{area, addr, tags::Address::kNoBit};
    point.type = type;
    point.registerType = regType;
    point.registerAddress = addr;
    return point;
}

}  // namespace

TEST(ModbusMappingTest, RegisterCountMatchesType) {
    EXPECT_EQ(makeRegisterPoint(0, tags::TypeId::Bool, RegisterType::Coil).registerCount(), 1u);
    EXPECT_EQ(makeRegisterPoint(0, tags::TypeId::Byte).registerCount(), 1u);
    EXPECT_EQ(makeRegisterPoint(0, tags::TypeId::Int).registerCount(), 1u);
    EXPECT_EQ(makeRegisterPoint(0, tags::TypeId::DInt).registerCount(), 2u);
    EXPECT_EQ(makeRegisterPoint(0, tags::TypeId::Real).registerCount(), 2u);
}

TEST(ModbusMappingTest, BuildBatchesCoalescesContiguousPoints) {
    std::vector<ModbusPointMapping> points = {
        makeRegisterPoint(0, tags::TypeId::Int),
        makeRegisterPoint(1, tags::TypeId::Int),
        makeRegisterPoint(2, tags::TypeId::DInt),
    };

    const auto batches = buildBatches(points);
    ASSERT_EQ(batches.size(), 1u);
    EXPECT_EQ(batches[0].startAddress, 0u);
    EXPECT_EQ(batches[0].quantity, 4u);  // 1 + 1 + 2 registers
    EXPECT_EQ(batches[0].points.size(), 3u);
}

TEST(ModbusMappingTest, BuildBatchesSplitsOnGap) {
    std::vector<ModbusPointMapping> points = {
        makeRegisterPoint(0, tags::TypeId::Int),
        makeRegisterPoint(5, tags::TypeId::Int),  // gap
    };

    const auto batches = buildBatches(points);
    ASSERT_EQ(batches.size(), 2u);
    EXPECT_EQ(batches[0].startAddress, 0u);
    EXPECT_EQ(batches[1].startAddress, 5u);
}

TEST(ModbusMappingTest, BuildBatchesGroupsByRegisterType) {
    std::vector<ModbusPointMapping> points = {
        makeRegisterPoint(0, tags::TypeId::Bool, RegisterType::Coil),
        makeRegisterPoint(0, tags::TypeId::Int, RegisterType::HoldingRegister),
    };

    const auto batches = buildBatches(points);
    ASSERT_EQ(batches.size(), 2u);
}

TEST(ModbusMappingTest, ValidateRejectsCoilWithNonBoolType) {
    ModbusDeviceConfig config;
    config.name = "dev";
    config.points.push_back(makeRegisterPoint(0, tags::TypeId::Int, RegisterType::Coil));
    EXPECT_THROW(validateDeviceConfig(config), std::invalid_argument);
}

TEST(ModbusMappingTest, ValidateRejectsHoldingRegisterWithBoolType) {
    ModbusDeviceConfig config;
    config.name = "dev";
    config.points.push_back(makeRegisterPoint(0, tags::TypeId::Bool, RegisterType::HoldingRegister));
    EXPECT_THROW(validateDeviceConfig(config), std::invalid_argument);
}

TEST(ModbusMappingTest, ValidateRejectsReadOnlyTableMappedToOutput) {
    ModbusDeviceConfig config;
    config.name = "dev";
    config.points.push_back(makeRegisterPoint(0, tags::TypeId::Int, RegisterType::InputRegister,
                                               tags::MemoryArea::Output));
    EXPECT_THROW(validateDeviceConfig(config), std::invalid_argument);
}

TEST(ModbusMappingTest, ValidateRejectsOverlappingRanges) {
    ModbusDeviceConfig config;
    config.name = "dev";
    config.points.push_back(makeRegisterPoint(0, tags::TypeId::DInt));  // occupies 0-1
    config.points.push_back(makeRegisterPoint(1, tags::TypeId::Int));   // overlaps at 1
    EXPECT_THROW(validateDeviceConfig(config), std::invalid_argument);
}

TEST(ModbusMappingTest, ValidateAcceptsWellFormedConfig) {
    ModbusDeviceConfig config;
    config.name = "dev";
    config.host = "127.0.0.1";
    config.points.push_back(makeRegisterPoint(0, tags::TypeId::Int));
    config.points.push_back(makeRegisterPoint(1, tags::TypeId::DInt));
    EXPECT_NO_THROW(validateDeviceConfig(config));
}

TEST(ModbusMappingTest, ValidateRejectsAddressOverflow) {
    ModbusDeviceConfig config;
    config.name = "dev";
    config.points.push_back(makeRegisterPoint(65535, tags::TypeId::DInt));  // needs 2 registers, overflows
    EXPECT_THROW(validateDeviceConfig(config), std::invalid_argument);
}
