#pragma once
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include "softplc/runtime/program_abi.h"
#include "softplc/st/value_ops.hpp"
namespace softplc::compiled {
using tags::Value;
using tags::TypeId;
using namespace st::ops;
struct Cancelled {};
struct Scope {
    PlcContext* c; const char* previous;
    Scope(PlcContext* c, const char* name) : c(c), previous(c->scope) { c->scope = name; }
    ~Scope() { c->scope = previous; }
};
inline void check(PlcContext* c) { if (c->api->checkpoint(c)) throw Cancelled{}; }
inline void network(PlcContext* c, uint32_t id, const Value& powered) {
    if (c->api->network(c, id, std::get<bool>(powered), c->scope)) throw Cancelled{};
}
inline PlcValue toAbi(const Value& value) {
    PlcValue v{}; v.type = static_cast<uint32_t>(tags::typeOf(value));
    std::visit([&](const auto& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::string>) {
            if (x.size() > 254) throw std::runtime_error("STRING exceeds 254 bytes");
            std::memcpy(v.text, x.data(), x.size());
        } else if constexpr (std::is_same_v<T, tags::TimeValue>) v.integer = x.count();
        else if constexpr (std::is_floating_point_v<T>) v.real = x;
        else v.integer = x;
    }, value);
    return v;
}
inline Value fromAbi(const PlcValue& v) {
    switch (v.type) {
        case PLC_BOOL: return v.integer != 0;
        case PLC_BYTE: return static_cast<uint8_t>(v.integer);
        case PLC_INT: return static_cast<int16_t>(v.integer);
        case PLC_DINT: return static_cast<int32_t>(v.integer);
        case PLC_REAL: return static_cast<float>(v.real);
        case PLC_LREAL: return v.real;
        case PLC_TIME: return tags::TimeValue(v.integer);
        case PLC_STRING: return std::string(v.text);
        default: throw std::runtime_error("invalid ABI type");
    }
}
inline Value read(PlcContext* c, uint32_t id) {
    PlcValue v{};
    if (c->api->read_tag(c, id, &v)) throw std::runtime_error("tag read failed");
    return fromAbi(v);
}
inline void write(PlcContext* c, uint32_t id, TypeId type, const Value& value) {
    const auto v = toAbi(coerceToType(value, type));
    if (c->api->write_tag(c, id, &v)) throw std::runtime_error("tag write failed");
}
inline bool truth(const Value& v) { return std::get<bool>(v); }
}
