#include "softplc/io/modbus_rtu_driver.hpp"

#include <stdexcept>
#include <thread>

namespace softplc::io {

ModbusRtuIoDriver::ModbusRtuIoDriver(std::vector<ModbusRtuBusConfig> busConfigs) {
    for (auto& busConfig : busConfigs) {
        validateRtuBusConfig(busConfig);

        auto bus = std::make_unique<Bus>();
        bus->config = busConfig;
        bus->client = std::make_unique<ModbusRtuBus>(busConfig);

        for (const auto& deviceConfig : busConfig.devices) {
            auto device = std::make_unique<RtuDevice>();
            device->config = deviceConfig;
            for (const auto& point : deviceConfig.points) {
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
            bus->devices.push_back(std::move(device));
        }

        buses_.push_back(std::move(bus));
    }
}

ModbusRtuIoDriver::~ModbusRtuIoDriver() { stop(); }

void ModbusRtuIoDriver::start() {
    for (auto& busPtr : buses_) {
        Bus* bus = busPtr.get();
        if (bus->thread.joinable()) continue;
        bus->thread = std::jthread([this, bus](std::stop_token stopToken) { pollBusLoop(*bus, stopToken); });
    }
}

void ModbusRtuIoDriver::stop() {
    for (auto& busPtr : buses_) {
        if (!busPtr->thread.joinable()) continue;
        busPtr->thread.request_stop();
        busPtr->thread.join();
    }
}

bool ModbusRtuIoDriver::isRunning() const {
    for (const auto& busPtr : buses_) {
        if (busPtr->thread.joinable()) return true;
    }
    return false;
}

std::size_t ModbusRtuIoDriver::deviceCount(std::size_t busIndex) const {
    if (busIndex >= buses_.size()) {
        throw std::out_of_range("ModbusRtuIoDriver::deviceCount: invalid bus index");
    }
    return buses_[busIndex]->devices.size();
}

ModbusDeviceDiagnostics ModbusRtuIoDriver::diagnostics(std::size_t busIndex, std::size_t deviceIndex) const {
    if (busIndex >= buses_.size()) {
        throw std::out_of_range("ModbusRtuIoDriver::diagnostics: invalid bus index");
    }
    const Bus& bus = *buses_[busIndex];
    if (deviceIndex >= bus.devices.size()) {
        throw std::out_of_range("ModbusRtuIoDriver::diagnostics: invalid device index");
    }
    const RtuDevice& device = *bus.devices[deviceIndex];
    std::lock_guard<std::mutex> lock(device.diagMutex);
    return device.diag;
}

void ModbusRtuIoDriver::readInputs(tags::TagStore& tags) {
    for (auto& busPtr : buses_) {
        for (auto& devicePtr : busPtr->devices) {
            RtuDevice& device = *devicePtr;

            {
                std::lock_guard<std::mutex> cacheLock(device.cacheMutex);
                for (std::size_t i = 0; i < device.inputPoints.size(); ++i) {
                    const CachedPoint& cached = device.inputCache[i];
                    if (!cached.valid) continue;

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
}

void ModbusRtuIoDriver::writeOutputs(const tags::TagStore& tags) {
    for (auto& busPtr : buses_) {
        for (auto& devicePtr : busPtr->devices) {
            RtuDevice& device = *devicePtr;
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
}

void ModbusRtuIoDriver::pollBusLoop(Bus& bus, std::stop_token stopToken) {
    while (!stopToken.stop_requested()) {
        pollBusOnce(bus);
        std::this_thread::sleep_for(bus.config.pollInterval);
    }
}

void ModbusRtuIoDriver::pollBusOnce(Bus& bus) {
    // Every device on this bus is polled sequentially, on this one thread -- never
    // concurrently, since they all share the same physical serial line.
    for (auto& devicePtr : bus.devices) {
        RtuDevice& device = *devicePtr;
        const std::uint8_t unitId = device.config.unitId;

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

        for (const ReadBatch& batch : device.readBatches) {
            std::vector<std::uint16_t> registers;
            std::vector<bool> bits;
            const RequestResult result = bus.client->readBatch(unitId, batch, registers, bits);
            recordOutcome(result);
            if (!result.ok()) continue;

            std::lock_guard<std::mutex> cacheLock(device.cacheMutex);
            for (const ModbusPointMapping* point : batch.points) {
                const auto idx = static_cast<std::size_t>(point - device.inputPoints.data());
                device.inputCache[idx].value = decodeModbusPointValue(*point, batch, registers, bits);
                device.inputCache[idx].valid = true;
            }
        }

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
                    if (cached.dirty || (now - cached.lastWriteTime) >= bus.config.outputRefreshInterval) {
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
                result = bus.client->writeCoils(unitId, batch.startAddress, bitsToWrite);
            } else {
                std::vector<std::uint16_t> registersToWrite(batch.quantity);
                for (std::size_t i = 0; i < batch.points.size(); ++i) {
                    const ModbusPointMapping* point = batch.points[i];
                    const auto offset =
                        static_cast<std::size_t>(point->registerAddress - batch.startAddress);
                    encodeModbusPointValue(*point, snapshot[i], offset, registersToWrite);
                }
                result = bus.client->writeRegisters(unitId, batch.startAddress, registersToWrite);
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
}

}  // namespace softplc::io
