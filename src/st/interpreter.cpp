#include "softplc/st/interpreter.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace softplc::st {

using tags::TypeId;
using tags::Value;

namespace {

int numericRank(TypeId t) {
    switch (t) {
        case TypeId::Byte:
            return 0;
        case TypeId::Int:
            return 1;
        case TypeId::DInt:
            return 2;
        case TypeId::Real:
            return 3;
        case TypeId::LReal:
            return 4;
        default:
            return -1;
    }
}

bool isNumeric(TypeId t) { return numericRank(t) >= 0; }

double asDouble(const Value& v) {
    return std::visit(
        [](auto&& x) -> double {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) {
                return static_cast<double>(x);
            } else {
                throw std::runtime_error("expected a numeric value in arithmetic expression");
            }
        },
        v);
}

Value fromDouble(double d, TypeId type) {
    switch (type) {
        case TypeId::Byte:
            return static_cast<std::uint8_t>(d);
        case TypeId::Int:
            return static_cast<std::int16_t>(d);
        case TypeId::DInt:
            return static_cast<std::int32_t>(d);
        case TypeId::Real:
            return static_cast<float>(d);
        case TypeId::LReal:
            return d;
        default:
            throw std::runtime_error("cannot produce a non-numeric result from arithmetic");
    }
}

TypeId numericResultType(TypeId a, TypeId b) {
    static constexpr std::array<TypeId, 5> kOrder = {TypeId::Byte, TypeId::Int, TypeId::DInt,
                                                       TypeId::Real, TypeId::LReal};
    return kOrder[static_cast<std::size_t>(std::max(numericRank(a), numericRank(b)))];
}

// Bare numeric literals (and FC/FB call-arg values) are evaluated without knowledge of
// the assignment target's declared type (LiteralExpr::value is always DInt for integer
// literals -- see Parser::parsePrimary), so every write through the interpreter must
// coerce to the target tag's declared type rather than assuming the evaluated Value's
// variant alternative already matches it.
Value coerceToType(const Value& v, TypeId target) {
    const TypeId source = tags::typeOf(v);
    if (source == target) return v;
    if (isNumeric(source) && isNumeric(target)) {
        return fromDouble(asDouble(v), target);
    }
    throw std::runtime_error(std::string("type mismatch: cannot assign ") + tags::toString(source) +
                              " value to " + tags::toString(target) + " target");
}

Value evalArithmetic(BinaryOp op, const Value& l, const Value& r) {
    const TypeId lt = tags::typeOf(l);
    const TypeId rt = tags::typeOf(r);

    if (lt == TypeId::Time && rt == TypeId::Time) {
        const auto lv = std::get<tags::TimeValue>(l);
        const auto rv = std::get<tags::TimeValue>(r);
        switch (op) {
            case BinaryOp::Add:
                return lv + rv;
            case BinaryOp::Sub:
                return lv - rv;
            default:
                throw std::runtime_error("TIME only supports + and -");
        }
    }

    if (!isNumeric(lt) || !isNumeric(rt)) {
        throw std::runtime_error("arithmetic operators require numeric (or TIME+TIME) operands");
    }

    const double a = asDouble(l);
    const double b = asDouble(r);
    double result = 0.0;
    switch (op) {
        case BinaryOp::Add:
            result = a + b;
            break;
        case BinaryOp::Sub:
            result = a - b;
            break;
        case BinaryOp::Mul:
            result = a * b;
            break;
        case BinaryOp::Div:
            if (b == 0.0) throw std::runtime_error("division by zero");
            result = a / b;
            break;
        default:
            throw std::runtime_error("not an arithmetic operator");
    }
    return fromDouble(result, numericResultType(lt, rt));
}

Value evalComparison(BinaryOp op, const Value& l, const Value& r) {
    const TypeId lt = tags::typeOf(l);
    const TypeId rt = tags::typeOf(r);

    if (lt == TypeId::Bool && rt == TypeId::Bool) {
        const bool a = std::get<bool>(l);
        const bool b = std::get<bool>(r);
        switch (op) {
            case BinaryOp::Eq:
                return a == b;
            case BinaryOp::Ne:
                return a != b;
            default:
                throw std::runtime_error("BOOL only supports = and <>");
        }
    }
    if (lt == TypeId::String && rt == TypeId::String) {
        const auto& a = std::get<std::string>(l);
        const auto& b = std::get<std::string>(r);
        switch (op) {
            case BinaryOp::Eq:
                return a == b;
            case BinaryOp::Ne:
                return a != b;
            case BinaryOp::Lt:
                return a < b;
            case BinaryOp::Gt:
                return a > b;
            case BinaryOp::Le:
                return a <= b;
            case BinaryOp::Ge:
                return a >= b;
            default:
                throw std::runtime_error("not a comparison operator");
        }
    }
    if (lt == TypeId::Time && rt == TypeId::Time) {
        const auto a = std::get<tags::TimeValue>(l);
        const auto b = std::get<tags::TimeValue>(r);
        switch (op) {
            case BinaryOp::Eq:
                return a == b;
            case BinaryOp::Ne:
                return a != b;
            case BinaryOp::Lt:
                return a < b;
            case BinaryOp::Gt:
                return a > b;
            case BinaryOp::Le:
                return a <= b;
            case BinaryOp::Ge:
                return a >= b;
            default:
                throw std::runtime_error("not a comparison operator");
        }
    }
    if (isNumeric(lt) && isNumeric(rt)) {
        const double a = asDouble(l);
        const double b = asDouble(r);
        switch (op) {
            case BinaryOp::Eq:
                return a == b;
            case BinaryOp::Ne:
                return a != b;
            case BinaryOp::Lt:
                return a < b;
            case BinaryOp::Gt:
                return a > b;
            case BinaryOp::Le:
                return a <= b;
            case BinaryOp::Ge:
                return a >= b;
            default:
                throw std::runtime_error("not a comparison operator");
        }
    }
    throw std::runtime_error("type mismatch in comparison");
}

Value evalLogical(BinaryOp op, const Value& l, const Value& r) {
    if (tags::typeOf(l) != TypeId::Bool || tags::typeOf(r) != TypeId::Bool) {
        throw std::runtime_error("AND/OR/XOR require BOOL operands");
    }
    const bool a = std::get<bool>(l);
    const bool b = std::get<bool>(r);
    switch (op) {
        case BinaryOp::And:
            return a && b;
        case BinaryOp::Or:
            return a || b;
        case BinaryOp::Xor:
            return a != b;
        default:
            throw std::runtime_error("not a logical operator");
    }
}

Value evalUnary(UnaryOp op, const Value& v) {
    if (op == UnaryOp::Not) {
        if (tags::typeOf(v) != TypeId::Bool) throw std::runtime_error("NOT requires a BOOL operand");
        return !std::get<bool>(v);
    }
    const TypeId t = tags::typeOf(v);
    if (t == TypeId::Time) {
        return -std::get<tags::TimeValue>(v);
    }
    if (!isNumeric(t)) throw std::runtime_error("unary - requires a numeric or TIME operand");
    return fromDouble(-asDouble(v), t);
}

}  // namespace

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

void Interpreter::run(tags::TagStore& tags) const { execBlock(program_.body, tags); }

}  // namespace softplc::st
