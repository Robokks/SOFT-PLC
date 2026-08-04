#include "softplc/io/modbus_mapping.hpp"

#include <algorithm>
#include <bit>
#include <map>
#include <stdexcept>

namespace softplc::io {

namespace {

bool isBitTable(RegisterType type) {
    return type == RegisterType::Coil || type == RegisterType::DiscreteInput;
}

bool isReadOnlyTable(RegisterType type) {
    return type == RegisterType::DiscreteInput || type == RegisterType::InputRegister;
}

std::map<RegisterType, std::vector<const ModbusPointMapping*>> groupByRegisterType(
    const std::vector<ModbusPointMapping>& points) {
    std::map<RegisterType, std::vector<const ModbusPointMapping*>> byType;
    for (const auto& point : points) {
        byType[point.registerType].push_back(&point);
    }
    for (auto& [type, pts] : byType) {
        std::sort(pts.begin(), pts.end(), [](const ModbusPointMapping* a, const ModbusPointMapping* b) {
            return a->registerAddress < b->registerAddress;
        });
    }
    return byType;
}

}  // namespace

std::uint16_t ModbusPointMapping::registerCount() const {
    switch (type) {
        case tags::TypeId::Bool:
            return 1;
        case tags::TypeId::Byte:
        case tags::TypeId::Int:
            return 1;
        case tags::TypeId::DInt:
        case tags::TypeId::Real:
            return 2;
        default:
            return 0;
    }
}

namespace {

// Shared by validateDeviceConfig() (TCP) and validateRtuBusConfig() (RTU, once per
// device on the bus): both ultimately validate "one set of points against one
// register/coil address space," identical regardless of transport. `contextName` is
// folded into every thrown message so callers keep their own wording (e.g.
// "ModbusDeviceConfig 'Foo'" vs "ModbusRtuDeviceConfig 'Foo' on bus 'Bar'").
void validatePoints(const std::string& contextName, const std::vector<ModbusPointMapping>& points) {
    for (const auto& point : points) {
        if (isBitTable(point.registerType)) {
            if (point.type != tags::TypeId::Bool) {
                throw std::invalid_argument(contextName + ": Coil/DiscreteInput points must be BOOL");
            }
        } else {
            switch (point.type) {
                case tags::TypeId::Byte:
                case tags::TypeId::Int:
                case tags::TypeId::DInt:
                case tags::TypeId::Real:
                    break;
                default:
                    throw std::invalid_argument(
                        contextName + ": HoldingRegister/InputRegister points must be BYTE, INT, DINT or REAL");
            }
        }

        if (isReadOnlyTable(point.registerType) && point.iecAddress.area == tags::MemoryArea::Output) {
            throw std::invalid_argument(
                contextName +
                ": DiscreteInput/InputRegister (read-only) cannot be mapped to a %Q output address");
        }

        const std::uint16_t count = point.registerCount();
        if (count == 0 ||
            static_cast<std::uint32_t>(point.registerAddress) + count > 65536u) {
            throw std::invalid_argument(contextName +
                                         ": point register range overflows the 16-bit address space");
        }
    }

    for (const auto& [type, groupedPoints] : groupByRegisterType(points)) {
        for (std::size_t i = 1; i < groupedPoints.size(); ++i) {
            const ModbusPointMapping* prev = groupedPoints[i - 1];
            const ModbusPointMapping* current = groupedPoints[i];
            const std::uint16_t prevEnd =
                static_cast<std::uint16_t>(prev->registerAddress + prev->registerCount());
            if (current->registerAddress < prevEnd) {
                throw std::invalid_argument(contextName + ": overlapping register ranges");
            }
        }
    }
}

}  // namespace

void validateDeviceConfig(const ModbusDeviceConfig& config) {
    validatePoints("ModbusDeviceConfig '" + config.name + "'", config.points);
}

void validateRtuBusConfig(const ModbusRtuBusConfig& bus) {
    if (bus.devicePath.empty()) {
        throw std::invalid_argument("ModbusRtuBusConfig '" + bus.name + "': devicePath must not be empty");
    }

    std::vector<std::uint8_t> unitIds;
    for (const auto& device : bus.devices) {
        validatePoints("ModbusRtuDeviceConfig '" + device.name + "' on bus '" + bus.name + "'",
                        device.points);
        unitIds.push_back(device.unitId);
    }

    std::sort(unitIds.begin(), unitIds.end());
    if (std::adjacent_find(unitIds.begin(), unitIds.end()) != unitIds.end()) {
        throw std::invalid_argument("ModbusRtuBusConfig '" + bus.name +
                                     "': two devices on the same bus cannot share a unit id");
    }
}

std::vector<ReadBatch> buildBatches(const std::vector<ModbusPointMapping>& points) {
    std::vector<ReadBatch> batches;

    for (const auto& [type, pts] : groupByRegisterType(points)) {
        const std::uint16_t maxQuantity = isBitTable(type) ? 2000 : 125;

        ReadBatch current;
        bool haveCurrent = false;
        for (const auto* point : pts) {
            const std::uint16_t count = point->registerCount();
            if (!haveCurrent) {
                current = ReadBatch{type, point->registerAddress, count, {point}};
                haveCurrent = true;
                continue;
            }

            const std::uint16_t expectedNext =
                static_cast<std::uint16_t>(current.startAddress + current.quantity);
            const std::uint16_t newQuantity = static_cast<std::uint16_t>(current.quantity + count);
            if (point->registerAddress == expectedNext && newQuantity <= maxQuantity) {
                current.quantity = newQuantity;
                current.points.push_back(point);
            } else {
                batches.push_back(std::move(current));
                current = ReadBatch{type, point->registerAddress, count, {point}};
            }
        }
        if (haveCurrent) {
            batches.push_back(std::move(current));
        }
    }

    return batches;
}

tags::Value decodeModbusPointValue(const ModbusPointMapping& point, const ReadBatch& batch,
                                    const std::vector<std::uint16_t>& registers,
                                    const std::vector<bool>& bits) {
    const std::size_t offset = static_cast<std::size_t>(point.registerAddress - batch.startAddress);

    if (point.registerType == RegisterType::Coil || point.registerType == RegisterType::DiscreteInput) {
        return bits.at(offset);
    }

    switch (point.type) {
        case tags::TypeId::Byte:
            return static_cast<std::uint8_t>(registers.at(offset) & 0xFF);
        case tags::TypeId::Int:
            return static_cast<std::int16_t>(registers.at(offset));
        case tags::TypeId::DInt:
        case tags::TypeId::Real: {
            const std::uint16_t a = registers.at(offset);
            const std::uint16_t b = registers.at(offset + 1);
            const std::uint16_t high = point.wordOrder == WordOrder::BigEndianWords ? a : b;
            const std::uint16_t low = point.wordOrder == WordOrder::BigEndianWords ? b : a;
            const std::uint32_t combined = (static_cast<std::uint32_t>(high) << 16) | low;
            if (point.type == tags::TypeId::DInt) {
                return static_cast<std::int32_t>(combined);
            }
            return std::bit_cast<float>(combined);
        }
        default:
            return tags::defaultValueFor(point.type);
    }
}

void encodeModbusPointValue(const ModbusPointMapping& point, const tags::Value& value, std::size_t offset,
                             std::vector<std::uint16_t>& registers) {
    switch (point.type) {
        case tags::TypeId::Byte:
            registers.at(offset) = static_cast<std::uint16_t>(std::get<std::uint8_t>(value));
            return;
        case tags::TypeId::Int:
            registers.at(offset) = static_cast<std::uint16_t>(std::get<std::int16_t>(value));
            return;
        case tags::TypeId::DInt:
        case tags::TypeId::Real: {
            std::uint32_t combined = 0;
            if (point.type == tags::TypeId::DInt) {
                combined = static_cast<std::uint32_t>(std::get<std::int32_t>(value));
            } else {
                combined = std::bit_cast<std::uint32_t>(std::get<float>(value));
            }
            const std::uint16_t high = static_cast<std::uint16_t>(combined >> 16);
            const std::uint16_t low = static_cast<std::uint16_t>(combined & 0xFFFFu);
            if (point.wordOrder == WordOrder::BigEndianWords) {
                registers.at(offset) = high;
                registers.at(offset + 1) = low;
            } else {
                registers.at(offset) = low;
                registers.at(offset + 1) = high;
            }
            return;
        }
        default:
            return;
    }
}

}  // namespace softplc::io
