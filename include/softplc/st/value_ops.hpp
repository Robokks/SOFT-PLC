#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include "softplc/st/ast.hpp"
namespace softplc::st::ops {
using tags::TypeId;
using tags::Value;


inline int numericRank(TypeId t) {
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

inline bool isNumeric(TypeId t) { return numericRank(t) >= 0; }

inline double asDouble(const Value& v) {
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

inline Value fromDouble(double d, TypeId type) {
    if (!std::isfinite(d)) throw std::runtime_error("non-finite arithmetic result");
    auto fits = [d](double lo, double hi) {
        if (d < lo || d > hi) throw std::runtime_error("numeric overflow");
    };
    if (type == TypeId::Byte) fits(0, 255);
    if (type == TypeId::Int) fits(-32768, 32767);
    if (type == TypeId::DInt) fits(-2147483648.0, 2147483647.0);
    if (type == TypeId::Real) fits(-std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
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

inline TypeId numericResultType(TypeId a, TypeId b) {
    static constexpr std::array<TypeId, 5> kOrder = {TypeId::Byte, TypeId::Int, TypeId::DInt,
                                                       TypeId::Real, TypeId::LReal};
    return kOrder[static_cast<std::size_t>(std::max(numericRank(a), numericRank(b)))];
}

// Bare numeric literals (and FC/FB call-arg values) are evaluated without knowledge of
// the assignment target's declared type (LiteralExpr::value is always DInt for integer
// literals -- see Parser::parsePrimary), so every write through the interpreter must
// coerce to the target tag's declared type rather than assuming the evaluated Value's
// variant alternative already matches it.
inline Value coerceToType(const Value& v, TypeId target) {
    const TypeId source = tags::typeOf(v);
    if (source == target) return v;
    if (isNumeric(source) && isNumeric(target)) {
        return fromDouble(asDouble(v), target);
    }
    throw std::runtime_error(std::string("type mismatch: cannot assign ") + tags::toString(source) +
                              " value to " + tags::toString(target) + " target");
}

inline Value evalArithmetic(BinaryOp op, const Value& l, const Value& r) {
    const TypeId lt = tags::typeOf(l);
    const TypeId rt = tags::typeOf(r);

    if (lt == TypeId::Time && rt == TypeId::Time) {
        const auto lv = std::get<tags::TimeValue>(l);
        const auto rv = std::get<tags::TimeValue>(r);
        const auto a=lv.count(), b=rv.count();
        const auto lo=std::numeric_limits<int64_t>::min(), hi=std::numeric_limits<int64_t>::max();
        switch (op) {
            case BinaryOp::Add:
                if ((b>0 && a>hi-b) || (b<0 && a<lo-b)) throw std::runtime_error("TIME overflow");
                return lv + rv;
            case BinaryOp::Sub:
                if ((b>0 && a<lo+b) || (b<0 && a>hi+b)) throw std::runtime_error("TIME overflow");
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

inline Value evalComparison(BinaryOp op, const Value& l, const Value& r) {
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

inline Value evalLogical(BinaryOp op, const Value& l, const Value& r) {
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

inline Value evalUnary(UnaryOp op, const Value& v) {
    if (op == UnaryOp::Not) {
        if (tags::typeOf(v) != TypeId::Bool) throw std::runtime_error("NOT requires a BOOL operand");
        return !std::get<bool>(v);
    }
    const TypeId t = tags::typeOf(v);
    if (t == TypeId::Time) {
        const auto n=std::get<tags::TimeValue>(v);
        if(n.count()==std::numeric_limits<int64_t>::min()) throw std::runtime_error("TIME overflow");
        return -n;
    }
    if (!isNumeric(t)) throw std::runtime_error("unary - requires a numeric or TIME operand");
    return fromDouble(-asDouble(v), t);
}


} // namespace softplc::st::ops
