#pragma once

#include <chrono>
#include <cstdint>
#include <string>
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

}  // namespace softplc::tags
