#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "softplc/net/serial_port.hpp"
#include "softplc/tags/tag.hpp"
#include "softplc/tags/value.hpp"

namespace softplc::io {

enum class RegisterType { Coil, DiscreteInput, HoldingRegister, InputRegister };

// Per-device polling health, shared by ModbusTcpIoDriver and ModbusRtuIoDriver (the
// underlying transport differs; what a caller wants to observe about a device's
// polling health doesn't).
struct ModbusDeviceDiagnostics {
    bool connected = false;
    std::uint64_t successfulPolls = 0;
    std::uint64_t failedPolls = 0;
    std::uint64_t consecutiveFailures = 0;
    std::string lastError;
    std::chrono::steady_clock::time_point lastSuccessTime{};
};

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

// One Modbus RTU slave (a "unit id") on a shared serial bus. Unlike ModbusDeviceConfig
// (one whole TCP connection per device), a bus's physical serial port is shared by
// every device declared on it -- see ModbusRtuBusConfig below.
struct ModbusRtuDeviceConfig {
    std::string name;
    std::uint8_t unitId = 1;
    // Optional BOOL tag written every poll: true while this specific slave is
    // responding, false otherwise -- same freeze-last-good-value health semantics as
    // ModbusDeviceConfig::healthTagAddress, just scoped to one slave rather than the
    // whole (here, shared) connection.
    std::optional<tags::Address> healthTagAddress;
    std::vector<ModbusPointMapping> points;
};

// One physical RS-485/RS-232 bus: exactly one serial port, shared (sequentially,
// never concurrently -- a serial line is a half-duplex medium) across every device
// declared in `devices`, each addressed per-request by its own unitId. This is why
// RTU gets one poller thread per *bus* rather than the one-per-*device* model
// ModbusTcpIoDriver uses for TCP, where each device really does own an independent
// socket.
struct ModbusRtuBusConfig {
    std::string name;
    std::string devicePath;  // e.g. "/dev/ttyUSB0" (POSIX) or "COM3" (Windows)
    net::SerialConfig serial;
    std::chrono::milliseconds requestTimeout{500};
    // Applies to one full pass over every device on the bus, not per-device (a bus
    // poll cycle already visits every device sequentially each cycle).
    std::chrono::milliseconds pollInterval{50};
    std::chrono::milliseconds reconnectBackoff{1000};
    std::chrono::milliseconds outputRefreshInterval{1000};
    std::vector<ModbusRtuDeviceConfig> devices;
};

// Throws std::invalid_argument for a fundamentally broken bus config: an empty device
// path, any device's points failing the same checks validateDeviceConfig() applies,
// or two devices on the bus sharing a unit id (would make them indistinguishable on
// the wire). Called once at construction time.
void validateRtuBusConfig(const ModbusRtuBusConfig& bus);

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

// Decodes one point's value out of a just-read batch's raw registers/bits (word-order-
// and type-aware -- see WordOrder/ModbusPointMapping above). Shared by every driver
// that reads via buildBatches()' batches (ModbusTcpIoDriver, ModbusRtuIoDriver) so the
// decode logic can't drift between transports.
[[nodiscard]] tags::Value decodeModbusPointValue(const ModbusPointMapping& point, const ReadBatch& batch,
                                                  const std::vector<std::uint16_t>& registers,
                                                  const std::vector<bool>& bits);

// Encodes `value` into `registers` at `offset` within a batch being assembled for a
// write request. The inverse of decodeModbusPointValue(); same word-order handling.
void encodeModbusPointValue(const ModbusPointMapping& point, const tags::Value& value, std::size_t offset,
                             std::vector<std::uint16_t>& registers);

}  // namespace softplc::io
