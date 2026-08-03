#pragma once

#include <map>
#include <mutex>
#include <optional>

#include "softplc/io/io_driver.hpp"
#include "softplc/tags/tag.hpp"
#include "softplc/tags/value.hpp"

namespace softplc::io {

// Phase 1 I/O backend with no real hardware: inputs are whatever a test/harness sets
// via setSimulatedInput(); outputs are recorded so callers can assert on what the
// program wrote. Shaped identically to a future real driver (e.g. Modbus/GPIO) so
// swapping it out requires no engine changes.
class SimulatedIoDriver : public IIoDriver {
public:
    void readInputs(tags::TagStore& tags) override;
    void writeOutputs(const tags::TagStore& tags) override;

    [[nodiscard]] std::string_view name() const override { return "SimulatedIoDriver"; }

    // Test/harness hook: forces the value that the next readInputs() will push into
    // the tag at this address.
    void setSimulatedInput(tags::Address addr, tags::Value value);

    // Test hook: last value observed for an output address, if writeOutputs() has run.
    [[nodiscard]] std::optional<tags::Value> lastOutput(tags::Address addr) const;

private:
    mutable std::mutex mutex_;
    std::map<tags::Address, tags::Value> simulatedInputs_;
    std::map<tags::Address, tags::Value> lastOutputs_;
};

}  // namespace softplc::io
