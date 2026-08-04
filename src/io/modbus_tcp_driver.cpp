#include "softplc/io/modbus_tcp_driver.hpp"

#include <stdexcept>
#include <thread>

namespace softplc::io {

ModbusTcpIoDriver::ModbusTcpIoDriver(std::vector<ModbusDeviceConfig> deviceConfigs) {
    for (auto& config : deviceConfigs) {
        validateDeviceConfig(config);

        auto device = std::make_unique<Device>();
        device->config = config;
        for (const auto& point : config.points) {
            if (point.iecAddress.area == tags::MemoryArea::Output) {
                device->outputPoints.push_back(point);
            } else {
                device->inputPoints.push_back(point);
            }
        }
        device->readBatches = buildBatches(device->inputPoints);
        device->writeBatches = buildBatches(device->outputPoints);
        device->inputCache.resize(device->inputPoints.size());
        device->outputCache.resize(device->outputPoints.size());
        device->client = std::make_unique<ModbusClient>(config);

        devices_.push_back(std::move(device));
    }
}

ModbusTcpIoDriver::~ModbusTcpIoDriver() { stop(); }

void ModbusTcpIoDriver::start() {
    for (auto& devicePtr : devices_) {
        Device* device = devicePtr.get();
        if (device->thread.joinable()) continue;
        device->thread = std::jthread(
            [this, device](std::stop_token stopToken) { pollDeviceLoop(*device, stopToken); });
    }
}

void ModbusTcpIoDriver::stop() {
    for (auto& devicePtr : devices_) {
        if (!devicePtr->thread.joinable()) continue;
        devicePtr->thread.request_stop();
        devicePtr->thread.join();
    }
}

bool ModbusTcpIoDriver::isRunning() const {
    for (const auto& devicePtr : devices_) {
        if (devicePtr->thread.joinable()) return true;
    }
    return false;
}

ModbusDeviceDiagnostics ModbusTcpIoDriver::diagnostics(std::size_t deviceIndex) const {
    if (deviceIndex >= devices_.size()) {
        throw std::out_of_range("ModbusTcpIoDriver::diagnostics: invalid device index");
    }
    const Device& device = *devices_[deviceIndex];
    std::lock_guard<std::mutex> lock(device.diagMutex);
    return device.diag;
}

void ModbusTcpIoDriver::readInputs(tags::TagStore& tags) {
    for (auto& devicePtr : devices_) {
        Device& device = *devicePtr;

        {
            std::lock_guard<std::mutex> cacheLock(device.cacheMutex);
            for (std::size_t i = 0; i < device.inputPoints.size(); ++i) {
                const CachedPoint& cached = device.inputCache[i];
                if (!cached.valid) continue;  // never successfully polled: leave tag at its default

                const ModbusPointMapping& point = device.inputPoints[i];
                const auto tagId = tags.find(point.iecAddress);
                if (!tagId) continue;
                if (tags.typeOf(*tagId) != point.type) continue;
                tags.write(*tagId, cached.value);
            }
        }

        if (device.config.healthTagAddress) {
            const auto healthId = tags.find(*device.config.healthTagAddress);
            if (healthId && tags.typeOf(*healthId) == tags::TypeId::Bool) {
                std::lock_guard<std::mutex> diagLock(device.diagMutex);
                tags.write(*healthId, device.diag.connected);
            }
        }
    }
}

void ModbusTcpIoDriver::writeOutputs(const tags::TagStore& tags) {
    for (auto& devicePtr : devices_) {
        Device& device = *devicePtr;
        std::lock_guard<std::mutex> cacheLock(device.cacheMutex);
        for (std::size_t i = 0; i < device.outputPoints.size(); ++i) {
            const ModbusPointMapping& point = device.outputPoints[i];
            const auto tagId = tags.find(point.iecAddress);
            if (!tagId) continue;
            if (tags.typeOf(*tagId) != point.type) continue;

            const tags::Value current = tags.read(*tagId);
            CachedPoint& cached = device.outputCache[i];
            if (!cached.valid || cached.value != current) {
                cached.value = current;
                cached.dirty = true;
            }
            cached.valid = true;
        }
    }
}

void ModbusTcpIoDriver::pollDeviceLoop(Device& device, std::stop_token stopToken) {
    while (!stopToken.stop_requested()) {
        pollOnce(device);
        std::this_thread::sleep_for(device.config.pollInterval);
    }
}

void ModbusTcpIoDriver::pollOnce(Device& device) {
    auto recordOutcome = [&](const RequestResult& result) {
        std::lock_guard<std::mutex> diagLock(device.diagMutex);
        if (result.ok()) {
            device.diag.connected = true;
            device.diag.successfulPolls++;
            device.diag.consecutiveFailures = 0;
            device.diag.lastSuccessTime = std::chrono::steady_clock::now();
        } else {
            device.diag.failedPolls++;
            device.diag.consecutiveFailures++;
            device.diag.lastError = result.detail;
            if (result.outcome != RequestOutcome::ModbusException) {
                device.diag.connected = false;
            }
        }
    };

    // Reads: on success, decode and update inputCache (freeze-last-good-value: a
    // failed batch leaves the cache exactly as it was).
    for (const ReadBatch& batch : device.readBatches) {
        std::vector<std::uint16_t> registers;
        std::vector<bool> bits;
        const RequestResult result = device.client->readBatch(batch, registers, bits);
        recordOutcome(result);
        if (!result.ok()) continue;

        std::lock_guard<std::mutex> cacheLock(device.cacheMutex);
        for (const ModbusPointMapping* point : batch.points) {
            const auto idx = static_cast<std::size_t>(point - device.inputPoints.data());
            device.inputCache[idx].value = decodeModbusPointValue(*point, batch, registers, bits);
            device.inputCache[idx].valid = true;
        }
    }

    // Writes: only for batches with at least one dirty or refresh-due point: build a
    // snapshot of the whole batch's current desired values, and send them all as one
    // write request.
    const auto now = std::chrono::steady_clock::now();
    for (const ReadBatch& batch : device.writeBatches) {
        bool needsWrite = false;
        std::vector<tags::Value> snapshot(batch.points.size());
        {
            std::lock_guard<std::mutex> cacheLock(device.cacheMutex);
            for (std::size_t i = 0; i < batch.points.size(); ++i) {
                const auto idx =
                    static_cast<std::size_t>(batch.points[i] - device.outputPoints.data());
                const CachedPoint& cached = device.outputCache[idx];
                if (!cached.valid) continue;
                snapshot[i] = cached.value;
                if (cached.dirty || (now - cached.lastWriteTime) >= device.config.outputRefreshInterval) {
                    needsWrite = true;
                }
            }
        }
        if (!needsWrite) continue;

        RequestResult result;
        if (batch.registerType == RegisterType::Coil) {
            std::vector<bool> bitsToWrite(batch.points.size());
            for (std::size_t i = 0; i < batch.points.size(); ++i) {
                bitsToWrite[i] = std::get<bool>(snapshot[i]);
            }
            result = device.client->writeCoils(batch.startAddress, bitsToWrite);
        } else {
            std::vector<std::uint16_t> registersToWrite(batch.quantity);
            for (std::size_t i = 0; i < batch.points.size(); ++i) {
                const ModbusPointMapping* point = batch.points[i];
                const auto offset = static_cast<std::size_t>(point->registerAddress - batch.startAddress);
                encodeModbusPointValue(*point, snapshot[i], offset, registersToWrite);
            }
            result = device.client->writeRegisters(batch.startAddress, registersToWrite);
        }
        recordOutcome(result);

        if (result.ok()) {
            std::lock_guard<std::mutex> cacheLock(device.cacheMutex);
            for (const ModbusPointMapping* point : batch.points) {
                const auto idx = static_cast<std::size_t>(point - device.outputPoints.data());
                device.outputCache[idx].dirty = false;
                device.outputCache[idx].lastWriteTime = now;
            }
        }
    }
}

}  // namespace softplc::io
