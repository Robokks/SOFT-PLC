#pragma once

#include "softplc/st/ast.hpp"
#include "softplc/tags/tag_store.hpp"

namespace softplc::st {

// Tree-walking evaluator for a bound StProgramAst — identifiers must already be
// resolved to TagIds (by StProgram::load) before run() is called. Safe to invoke
// once per scan cycle; holds no mutable state of its own beyond the tag store.
class Interpreter {
public:
    explicit Interpreter(const StProgramAst& program) : program_(program) {}

    void run(tags::TagStore& tags) const;

private:
    tags::Value evaluate(const Expr& expr, const tags::TagStore& tags) const;
    void execStmt(const Stmt& stmt, tags::TagStore& tags) const;
    void execBlock(const StmtList& stmts, tags::TagStore& tags) const;
    // Runs an FB instance's or FC call site's bound Frame: applies VAR_TEMP resets,
    // copies ':=' inputs in, executes the frame's body, copies '=>' outputs out.
    void execCall(const CallStmt& call, tags::TagStore& tags) const;
    // Evaluates a rung's condition once and drives each output coil: Direct writes
    // the condition value every scan; Set/Reset only ever force TRUE/FALSE when the
    // condition holds, otherwise leaving the coil's current value untouched (a
    // latch), matching standard ladder Set/Reset coil semantics.
    void execRung(const RungStmt& rung, tags::TagStore& tags) const;

    const StProgramAst& program_;
};

}  // namespace softplc::st
