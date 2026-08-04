#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "softplc/io/io_driver.hpp"
#include "softplc/io/modbus_mapping.hpp"
#include "softplc/io/modbus_rtu_bus.hpp"
#include "softplc/tags/tag_store.hpp"

namespace softplc::io {

// Modbus RTU (serial) client (master) IIoDriver: polls one or more configured buses,
// each on its own dedicated background thread (never the scan thread), feeding %I/%Q
// tags exactly like ModbusTcpIoDriver. The key structural difference from TCP: one
// poller thread per *bus*, not per device -- a serial line is a shared half-duplex
// medium, so every unit id configured on the same ModbusRtuBusConfig is polled
// sequentially by that bus's single thread each cycle, never concurrently. See
// docs/architecture.md's "Modbus RTU I/O driver" section for the full rationale.
class ModbusRtuIoDriver : public IIoDriver {
public:
    explicit ModbusRtuIoDriver(std::vector<ModbusRtuBusConfig> buses);
    ~ModbusRtuIoDriver() override;

    ModbusRtuIoDriver(const ModbusRtuIoDriver&) = delete;
    ModbusRtuIoDriver& operator=(const ModbusRtuIoDriver&) = delete;

    void readInputs(tags::TagStore& tags) override;
    void writeOutputs(const tags::TagStore& tags) override;
    [[nodiscard]] std::string_view name() const override { return "ModbusRtuIoDriver"; }

    // Starts one poller thread per configured bus. No-op if already running.
    void start();
    // Stops all poller threads and joins them. No-op if not running.
    void stop();
    [[nodiscard]] bool isRunning() const;

    [[nodiscard]] std::size_t busCount() const { return buses_.size(); }
    // Throws std::out_of_range for an invalid index.
    [[nodiscard]] std::size_t deviceCount(std::size_t busIndex) const;
    [[nodiscard]] ModbusDeviceDiagnostics diagnostics(std::size_t busIndex, std::size_t deviceIndex) const;

private:
    struct CachedPoint {
        tags::Value value;
        bool valid = false;
        bool dirty = false;
        std::chrono::steady_clock::time_point lastWriteTime{};
    };

    struct RtuDevice {
        ModbusRtuDeviceConfig config;

        std::vector<ModbusPointMapping> inputPoints;
        std::vector<ModbusPointMapping> outputPoints;
        std::vector<ReadBatch> readBatches;
        std::vector<ReadBatch> writeBatches;

        mutable std::mutex cacheMutex;
        std::vector<CachedPoint> inputCache;
        std::vector<CachedPoint> outputCache;

        mutable std::mutex diagMutex;
        ModbusDeviceDiagnostics diag;
    };

    struct Bus {
        ModbusRtuBusConfig config;
        std::unique_ptr<ModbusRtuBus> client;
        // Stable storage: readBatches/writeBatches inside each RtuDevice hold raw
        // pointers into that same device's point vectors, so devices must never be
        // resized after construction (matches ModbusTcpIoDriver::Device's contract).
        std::vector<std::unique_ptr<RtuDevice>> devices;
        std::jthread thread;
    };

    void pollBusLoop(Bus& bus, std::stop_token stopToken);
    // One full pass over every device on the bus, sequentially.
    void pollBusOnce(Bus& bus);

    std::vector<std::unique_ptr<Bus>> buses_;
};

}  // namespace softplc::io
