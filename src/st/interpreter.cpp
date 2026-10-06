#include "softplc/st/interpreter.hpp"
#include "softplc/st/value_ops.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace softplc::st {

using tags::TypeId;
using tags::Value;

using namespace ops;



Value Interpreter::evaluate(const Expr& expr, const tags::TagStore& tags) const {
    switch (expr.kind) {
        case ExprKind::Literal:
            return static_cast<const LiteralExpr&>(expr).value;
        case ExprKind::Identifier: {
            const auto& id = static_cast<const IdentifierExpr&>(expr);
            if (id.tagId == tags::kInvalidTagId) {
                throw std::runtime_error("unresolved identifier: " + id.name);
            }
            return tags.read(id.tagId);
        }
        case ExprKind::Unary: {
            const auto& u = static_cast<const UnaryExpr&>(expr);
            return evalUnary(u.op, evaluate(*u.operand, tags));
        }
        case ExprKind::Binary: {
            const auto& b = static_cast<const BinaryExpr&>(expr);
            const Value lv = evaluate(*b.lhs, tags);
            const Value rv = evaluate(*b.rhs, tags);
            switch (b.op) {
                case BinaryOp::Add:
                case BinaryOp::Sub:
                case BinaryOp::Mul:
                case BinaryOp::Div:
                    return evalArithmetic(b.op, lv, rv);
                case BinaryOp::Eq:
                case BinaryOp::Ne:
                case BinaryOp::Lt:
                case BinaryOp::Gt:
                case BinaryOp::Le:
                case BinaryOp::Ge:
                    return evalComparison(b.op, lv, rv);
                case BinaryOp::And:
                case BinaryOp::Or:
                case BinaryOp::Xor:
                    return evalLogical(b.op, lv, rv);
            }
        }
    }
    throw std::logic_error("Interpreter::evaluate: unreachable expression kind");
}

void Interpreter::execStmt(const Stmt& stmt, tags::TagStore& tags) const {
    switch (stmt.kind) {
        case StmtKind::Assign: {
            const auto& assign = static_cast<const AssignStmt&>(stmt);
            if (assign.targetId == tags::kInvalidTagId) {
                throw std::runtime_error("unresolved assignment target: " + assign.target);
            }
            tags.write(assign.targetId,
                       coerceToType(evaluate(*assign.value, tags), tags.typeOf(assign.targetId)));
            return;
        }
        case StmtKind::If: {
            const auto& ifStmt = static_cast<const IfStmt&>(stmt);
            for (const auto& branch : ifStmt.branches) {
                const Value cond = evaluate(*branch.condition, tags);
                if (tags::typeOf(cond) != TypeId::Bool) {
                    throw std::runtime_error("IF/ELSIF condition must be BOOL");
                }
                if (std::get<bool>(cond)) {
                    execBlock(branch.body, tags);
                    return;
                }
            }
            execBlock(ifStmt.elseBody, tags);
            return;
        }
        case StmtKind::While: {
            const auto& whileStmt = static_cast<const WhileStmt&>(stmt);
            constexpr std::uint64_t kMaxIterations = 1'000'000;
            std::uint64_t iterations = 0;
            while (true) {
                const Value cond = evaluate(*whileStmt.condition, tags);
                if (tags::typeOf(cond) != TypeId::Bool) {
                    throw std::runtime_error("WHILE condition must be BOOL");
                }
                if (!std::get<bool>(cond)) break;
                execBlock(whileStmt.body, tags);
                if (++iterations > kMaxIterations) {
                    throw std::runtime_error(
                        "WHILE loop exceeded max iterations (possible infinite loop)");
                }
            }
            return;
        }
        case StmtKind::Call: {
            execCall(static_cast<const CallStmt&>(stmt), tags);
            return;
        }
        case StmtKind::Rung: {
            execRung(static_cast<const RungStmt&>(stmt), tags);
            return;
        }
    }
    throw std::logic_error("Interpreter::execStmt: unreachable statement kind");
}

void Interpreter::execBlock(const StmtList& stmts, tags::TagStore& tags) const {
    for (const auto& stmt : stmts) {
        execStmt(*stmt, tags);
    }
}

void Interpreter::execCall(const CallStmt& call, tags::TagStore& tags) const {
    if (call.frameIndex == kInvalidFrame || call.frameIndex >= program_.frames.size()) {
        throw std::runtime_error("unresolved call target: " + call.calleeName);
    }
    const Frame& frame = program_.frames[call.frameIndex];

    // VAR_TEMP is scratch space: reset before every execution of this frame, never
    // retained across calls (unlike VAR_INPUT, which keeps its last value if unbound
    // this call, and unlike an FB instance's persistent VAR).
    for (const auto& [tagId, resetValue] : frame.tempResets) {
        tags.write(tagId, resetValue);
    }

    for (const auto& arg : call.args) {
        if (!arg.isOutput) {
            tags.write(arg.paramTagId,
                       coerceToType(evaluate(*arg.inputExpr, tags), tags.typeOf(arg.paramTagId)));
        }
    }

    execBlock(frame.body, tags);

    for (const auto& arg : call.args) {
        if (arg.isOutput) {
            tags.write(arg.outputTargetId,
                       coerceToType(tags.read(arg.paramTagId), tags.typeOf(arg.outputTargetId)));
        }
    }
}

void Interpreter::execRung(const RungStmt& rung, tags::TagStore& tags) const {
    const Value cond = evaluate(*rung.condition, tags);
    if (tags::typeOf(cond) != TypeId::Bool) {
        throw std::runtime_error("RUNG condition must evaluate to BOOL");
    }
    const bool powered = std::get<bool>(cond);

    for (const auto& out : rung.outputs) {
        switch (out.kind) {
            case RungOutputKind::Direct:
                tags.write(out.targetId, powered);
                break;
            case RungOutputKind::Set:
                if (powered) tags.write(out.targetId, true);
                break;
            case RungOutputKind::Reset:
                if (powered) tags.write(out.targetId, false);
                break;
        }
    }
}

void Interpreter::run(tags::TagStore& tags) const { execBlock(program_.body, tags); }

}  // namespace softplc::st
