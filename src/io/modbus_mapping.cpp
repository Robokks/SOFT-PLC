#include "softplc/io/modbus_mapping.hpp"

#include <algorithm>
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

void validateDeviceConfig(const ModbusDeviceConfig& config) {
    for (const auto& point : config.points) {
        if (isBitTable(point.registerType)) {
            if (point.type != tags::TypeId::Bool) {
                throw std::invalid_argument("ModbusDeviceConfig '" + config.name +
                                             "': Coil/DiscreteInput points must be BOOL");
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
                        "ModbusDeviceConfig '" + config.name +
                        "': HoldingRegister/InputRegister points must be BYTE, INT, DINT or REAL");
            }
        }

        if (isReadOnlyTable(point.registerType) && point.iecAddress.area == tags::MemoryArea::Output) {
            throw std::invalid_argument(
                "ModbusDeviceConfig '" + config.name +
                "': DiscreteInput/InputRegister (read-only) cannot be mapped to a %Q output address");
        }

        const std::uint16_t count = point.registerCount();
        if (count == 0 ||
            static_cast<std::uint32_t>(point.registerAddress) + count > 65536u) {
            throw std::invalid_argument("ModbusDeviceConfig '" + config.name +
                                         "': point register range overflows the 16-bit address space");
        }
    }

    for (const auto& [type, points] : groupByRegisterType(config.points)) {
        for (std::size_t i = 1; i < points.size(); ++i) {
            const ModbusPointMapping* prev = points[i - 1];
            const ModbusPointMapping* current = points[i];
            const std::uint16_t prevEnd =
                static_cast<std::uint16_t>(prev->registerAddress + prev->registerCount());
            if (current->registerAddress < prevEnd) {
                throw std::invalid_argument("ModbusDeviceConfig '" + config.name +
                                             "': overlapping register ranges");
            }
        }
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

}  // namespace softplc::io
