#pragma once

#include "softplc/st/ast.hpp"
#include "softplc/tags/tag_store.hpp"

namespace softplc::st {

// Binds a parsed CompilationUnit's PROGRAM against `tags`: declares DATA_BLOCK
// members, recursively expands FB instances and FC call sites (cloning each callee
// body once per instance/call-site -- see ast.hpp's Frame/clone() docs -- and
// resolving every identifier to a concrete TagId), and returns the fully-bound
// StProgramAst ready for Interpreter::run().
//
// Throws std::runtime_error for any binding failure: an undeclared identifier, an
// unknown instance/callee type, a call-arg naming an unknown parameter or binding it
// in the wrong direction, a required FUNCTION input left unbound, a duplicate
// FUNCTION_BLOCK/FUNCTION name, or a circular FB/FC instantiation.
[[nodiscard]] StProgramAst bindCompilationUnit(CompilationUnit unit, tags::TagStore& tags);

}  // namespace softplc::st
