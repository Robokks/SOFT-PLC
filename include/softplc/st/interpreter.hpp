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

    const StProgramAst& program_;
};

}  // namespace softplc::st
