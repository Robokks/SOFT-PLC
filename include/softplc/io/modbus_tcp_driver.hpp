#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "softplc/io/io_driver.hpp"
#include "softplc/io/modbus_client.hpp"
#include "softplc/io/modbus_mapping.hpp"
#include "softplc/tags/tag_store.hpp"

namespace softplc::io {

struct ModbusDeviceDiagnostics {
    bool connected = false;
    std::uint64_t successfulPolls = 0;
    std::uint64_t failedPolls = 0;
    std::uint64_t consecutiveFailures = 0;
    std::string lastError;
    std::chrono::steady_clock::time_point lastSuccessTime{};
};

// Modbus TCP client (master) IIoDriver: polls one or more configured devices on
// dedicated background threads (one per device, never the scan thread) and feeds
// %I/%Q tags, as a drop-in alternative to SimulatedIoDriver. readInputs()/
// writeOutputs() only ever copy between each device's in-memory cache and the
// TagStore -- never network I/O -- so a slow or unresponsive device can never stall
// a scan cycle. See docs/architecture.md's "Modbus TCP I/O driver" section for the
// full design rationale (freeze-last-good-value semantics, batching, etc.).
//
// Points are matched against tags already declared elsewhere (typically an ST
// program's `VAR ... AT %I0.0 ...` declarations), the same convention
// SimulatedIoDriver follows: this driver never declares tags itself, it looks them
// up by address each call.
class ModbusTcpIoDriver : public IIoDriver {
public:
    explicit ModbusTcpIoDriver(std::vector<ModbusDeviceConfig> devices);
    ~ModbusTcpIoDriver() override;

    ModbusTcpIoDriver(const ModbusTcpIoDriver&) = delete;
    ModbusTcpIoDriver& operator=(const ModbusTcpIoDriver&) = delete;

    void readInputs(tags::TagStore& tags) override;
    void writeOutputs(const tags::TagStore& tags) override;
    [[nodiscard]] std::string_view name() const override { return "ModbusTcpIoDriver"; }

    // Starts one poller thread per configured device. No-op if already running.
    void start();
    // Stops all poller threads and joins them. No-op if not running.
    void stop();
    [[nodiscard]] bool isRunning() const;

    [[nodiscard]] std::size_t deviceCount() const { return devices_.size(); }
    // Throws std::out_of_range for an invalid index.
    [[nodiscard]] ModbusDeviceDiagnostics diagnostics(std::size_t deviceIndex) const;

private:
    struct CachedPoint {
        // Input-direction points: last known-good value read from the device.
        // Output-direction points: last value observed from TagStore by
        // writeOutputs() -- the "desired" value the poller thread should push out.
        tags::Value value;
        bool valid = false;  // true once populated at least once
        bool dirty = false;  // output points only: true if not yet written to the device
        std::chrono::steady_clock::time_point lastWriteTime{};  // output points only
    };

    struct Device {
        ModbusDeviceConfig config;
        std::unique_ptr<ModbusClient> client;

        // Points partitioned by direction (Output address -> write; anything else
        // -> read). Stable storage: readBatches/writeBatches hold raw pointers into
        // these vectors, so they must never be resized after construction.
        std::vector<ModbusPointMapping> inputPoints;
        std::vector<ModbusPointMapping> outputPoints;
        std::vector<ReadBatch> readBatches;
        std::vector<ReadBatch> writeBatches;

        mutable std::mutex cacheMutex;
        std::vector<CachedPoint> inputCache;   // parallel to inputPoints
        std::vector<CachedPoint> outputCache;  // parallel to outputPoints

        mutable std::mutex diagMutex;
        ModbusDeviceDiagnostics diag;

        std::jthread thread;
    };

    void pollDeviceLoop(Device& device, std::stop_token stopToken);
    void pollOnce(Device& device);

    std::vector<std::unique_ptr<Device>> devices_;
};

}  // namespace softplc::io
