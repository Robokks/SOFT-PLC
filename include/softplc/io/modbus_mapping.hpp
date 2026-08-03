#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "softplc/tags/tag.hpp"
#include "softplc/tags/value.hpp"

namespace softplc::io {

enum class RegisterType { Coil, DiscreteInput, HoldingRegister, InputRegister };

// Word order across multi-register values (INT32/REAL spanning 2 registers) is not
// defined by the Modbus spec and varies by device vendor, so it's a per-point knob.
enum class WordOrder { BigEndianWords, LittleEndianWords };

// One tag <-> Modbus register/coil mapping. v1 deliberately restricts pairings: Coil
// and DiscreteInput only pair with BOOL; HoldingRegister and InputRegister only pair
// with BYTE/INT/DINT/REAL (BYTE uses one whole register, low 8 bits). Enforced by
// validateDeviceConfig(), not silently coerced.
struct ModbusPointMapping {
    tags::Address iecAddress;
    tags::TypeId type = tags::TypeId::Bool;
    RegisterType registerType = RegisterType::Coil;
    std::uint16_t registerAddress = 0;
    WordOrder wordOrder = WordOrder::BigEndianWords;

    // Bool -> 1 bit; Byte/Int -> 1 register; DInt/Real -> 2 registers. 0 for any
    // other (unsupported) type.
    [[nodiscard]] std::uint16_t registerCount() const;
};

struct ModbusDeviceConfig {
    std::string name;
    std::string host;
    std::uint16_t port = 502;
    std::uint8_t unitId = 1;
    std::chrono::milliseconds connectTimeout{500};
    std::chrono::milliseconds requestTimeout{200};
    std::chrono::milliseconds pollInterval{50};
    std::chrono::milliseconds reconnectBackoff{1000};
    std::chrono::milliseconds outputRefreshInterval{1000};
    // Optional BOOL tag written every poll: true while the device is connected and
    // responding, false otherwise, so ST logic can interlock on device health.
    std::optional<tags::Address> healthTagAddress;
    std::vector<ModbusPointMapping> points;
};

// Throws std::invalid_argument for a fundamentally broken config: an unsupported
// type/register-table pairing, a read-only table (DiscreteInput/InputRegister) mapped
// to a %Q address, overlapping register ranges within the device, or a point whose
// register range overflows the 16-bit address space. Called once at construction
// time; never called on the runtime/polling path.
void validateDeviceConfig(const ModbusDeviceConfig& config);

// One batched Modbus request: a contiguous run of registers/coils covering one or
// more points of the same RegisterType.
struct ReadBatch {
    RegisterType registerType = RegisterType::Coil;
    std::uint16_t startAddress = 0;
    std::uint16_t quantity = 0;
    std::vector<const ModbusPointMapping*> points;
};

// Coalesces strictly-contiguous (no gap) points per RegisterType into batches,
// capped at the Modbus protocol limit per request (2000 for Coil/DiscreteInput, 125
// for HoldingRegister/InputRegister). A gap of any size starts a new batch — bridging
// small gaps is a documented future optimization, not implemented here.
[[nodiscard]] std::vector<ReadBatch> buildBatches(const std::vector<ModbusPointMapping>& points);

}  // namespace softplc::io
