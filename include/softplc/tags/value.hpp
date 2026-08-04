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

// Numeric-type ranking (BYTE < INT < DINT < REAL < LREAL, i.e. widening order); -1 for
// a non-numeric TypeId (BOOL/TIME/STRING). Shared by the ST interpreter's binary-op
// result-type promotion and coerceToType()'s narrowing below, rather than duplicated.
int numericRank(TypeId type);
bool isNumeric(TypeId type);

// Reads any numeric Value alternative as a double (Interpreter's common arithmetic
// representation); throws std::runtime_error for a non-numeric Value.
double asDouble(const Value& value);
// Narrows/widens a double into the numeric Value alternative for `type`; throws
// std::runtime_error if `type` isn't numeric.
Value fromDouble(double d, TypeId type);

// Coerces `value` to `target`'s declared type when both are numeric (e.g. a bare
// integer literal -- always DINT, see Parser::parsePrimary -- assigned into an
// INT-declared tag) and returns `value` unchanged if it's already `target`'s type.
// Throws std::runtime_error for a genuine type mismatch (e.g. BOOL into DINT). Used by
// every write path that can't guarantee its source Value's variant already matches the
// destination tag's declared type: ST's AssignStmt/CallArg bindings (st/interpreter.cpp)
// and the HTTP tag-write endpoint (server/plc_server.cpp).
Value coerceToType(const Value& value, TypeId target);

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
