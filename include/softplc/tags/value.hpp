#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace softplc::tags {

// IEC 61131-3 elementary data types supported in Phase 1.
enum class TypeId {
    Bool,
    Byte,
    Int,
    DInt,
    Real,
    LReal,
    Time,
    String,
};

const char* toString(TypeId type);

// TIME values are represented as a duration (IEC literal T#5s -> 5000ms).
using TimeValue = std::chrono::milliseconds;

using Value = std::variant<bool, std::uint8_t, std::int16_t, std::int32_t, float, double,
                            TimeValue, std::string>;

TypeId typeOf(const Value& value);

// Default ("zero") value for a given type, used when a tag is declared without an initializer.
Value defaultValueFor(TypeId type);

// Escapes `s` for embedding inside a JSON string literal's quotes (quote/backslash/
// control characters). Shared by toJson() below and by any other JSON producer in this
// codebase (e.g. server/plc_server.cpp emitting tag names) rather than duplicated.
std::string jsonEscapeString(std::string_view s);

// Serializes a Value as a JSON value literal: `true`/`false` for BOOL, a bare number
// for the numeric types, a quoted/escaped string for STRING, and the millisecond count
// (a bare number, not a `T#...` literal) for TIME -- callers needing the IEC literal
// form already have the tag's TypeId available separately (toString(TypeId)) to
// interpret it.
std::string toJson(const Value& value);

}  // namespace softplc::tags
