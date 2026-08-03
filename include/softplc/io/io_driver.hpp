#pragma once

#include <string_view>

#include "softplc/tags/tag_store.hpp"

namespace softplc::io {

// Abstraction over a physical or simulated I/O backend. The scan engine calls
// readInputs() before program execution and writeOutputs() after, so implementations
// only need to move data between the tag store's %I/%Q areas and the real world.
class IIoDriver {
public:
    virtual ~IIoDriver() = default;

    virtual void readInputs(tags::TagStore& tags) = 0;
    virtual void writeOutputs(const tags::TagStore& tags) = 0;

    [[nodiscard]] virtual std::string_view name() const = 0;
};

}  // namespace softplc::io
