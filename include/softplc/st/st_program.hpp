#pragma once

#include <memory>
#include <string_view>

#include "softplc/core/program.hpp"
#include "softplc/core/scan_engine.hpp"
#include "softplc/st/ast.hpp"
#include "softplc/st/interpreter.hpp"
#include "softplc/tags/tag_store.hpp"

namespace softplc::st {

// Bridges the Structured Text pipeline into the core engine as an IProgram: parses
// source text, declares each VAR/VAR_INPUT/VAR_OUTPUT into the tag store, resolves
// every identifier reference to a TagId, and executes the bound AST each scan.
//
// Must be created through load() (which heap-allocates via make_shared) and is
// non-copyable/non-movable: the Interpreter member holds a reference to this
// object's own ast_ field, so the StProgram may never change address after
// construction.
class StProgram : public core::IProgram {
public:
    // Parses and binds `source` against `tags`, declaring its VAR blocks as new tags.
    // Throws LexError/ParseError on malformed source, or std::runtime_error if a
    // referenced identifier was never declared.
    static std::shared_ptr<StProgram> load(std::string_view source, tags::TagStore& tags);

    explicit StProgram(StProgramAst ast);

    StProgram(const StProgram&) = delete;
    StProgram& operator=(const StProgram&) = delete;
    StProgram(StProgram&&) = delete;
    StProgram& operator=(StProgram&&) = delete;

    void execute(core::ScanContext& ctx) override;
    [[nodiscard]] std::string_view name() const override;

private:
    StProgramAst ast_;
    Interpreter interpreter_;
};

}  // namespace softplc::st
