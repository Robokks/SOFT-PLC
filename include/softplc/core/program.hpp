#pragma once

#include <string_view>

namespace softplc::core {

struct ScanContext;

// An IEC 61131-3 Program Organization Unit (POU). The scan engine calls execute()
// once per scan cycle. Ladder/FBD/IL implementations are expected to implement this
// same interface in later phases.
class IProgram {
public:
    virtual ~IProgram() = default;

    virtual void execute(ScanContext& ctx) = 0;

    [[nodiscard]] virtual std::string_view name() const = 0;
};

}  // namespace softplc::core
