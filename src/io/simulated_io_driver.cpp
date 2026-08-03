#include "softplc/io/simulated_io_driver.hpp"

namespace softplc::io {

void SimulatedIoDriver::readInputs(tags::TagStore& tags) {
    std::lock_guard lock(mutex_);
    for (const auto& [addr, value] : simulatedInputs_) {
        if (auto id = tags.find(addr)) {
            tags.write(*id, value);
        }
    }
}

void SimulatedIoDriver::writeOutputs(const tags::TagStore& tags) {
    std::lock_guard lock(mutex_);
    tags.forEachInArea(tags::MemoryArea::Output, [this](const tags::Tag& tag) {
        lastOutputs_[*tag.address] = tag.value;
    });
}

void SimulatedIoDriver::setSimulatedInput(tags::Address addr, tags::Value value) {
    std::lock_guard lock(mutex_);
    simulatedInputs_[addr] = std::move(value);
}

std::optional<tags::Value> SimulatedIoDriver::lastOutput(tags::Address addr) const {
    std::lock_guard lock(mutex_);
    auto it = lastOutputs_.find(addr);
    if (it == lastOutputs_.end()) {
        return std::nullopt;
    }
    return it->second;
}

}  // namespace softplc::io
