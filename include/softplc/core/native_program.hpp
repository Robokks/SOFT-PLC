#pragma once

#include <functional>
#include <string>
#include <utility>

#include "softplc/core/program.hpp"

namespace softplc::core {

// Wraps an arbitrary C++ callable as an IProgram. Used for engine unit tests and for
// bootstrapping the runtime without going through the Structured Text pipeline.
class NativeProgram : public IProgram {
public:
    using Callback = std::function<void(ScanContext&)>;

    NativeProgram(std::string name, Callback callback)
        : name_(std::move(name)), callback_(std::move(callback)) {}

    void execute(ScanContext& ctx) override { callback_(ctx); }

    [[nodiscard]] std::string_view name() const override { return name_; }

private:
    std::string name_;
    Callback callback_;
};

}  // namespace softplc::core
