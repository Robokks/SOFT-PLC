#include "softplc/tags/value.hpp"

namespace softplc::tags {

const char* toString(TypeId type) {
    switch (type) {
        case TypeId::Bool:
            return "BOOL";
        case TypeId::Byte:
            return "BYTE";
        case TypeId::Int:
            return "INT";
        case TypeId::DInt:
            return "DINT";
        case TypeId::Real:
            return "REAL";
        case TypeId::LReal:
            return "LREAL";
        case TypeId::Time:
            return "TIME";
        case TypeId::String:
            return "STRING";
    }
    return "UNKNOWN";
}

TypeId typeOf(const Value& value) {
    switch (value.index()) {
        case 0:
            return TypeId::Bool;
        case 1:
            return TypeId::Byte;
        case 2:
            return TypeId::Int;
        case 3:
            return TypeId::DInt;
        case 4:
            return TypeId::Real;
        case 5:
            return TypeId::LReal;
        case 6:
            return TypeId::Time;
        default:
            return TypeId::String;
    }
}

Value defaultValueFor(TypeId type) {
    switch (type) {
        case TypeId::Bool:
            return false;
        case TypeId::Byte:
            return std::uint8_t{0};
        case TypeId::Int:
            return std::int16_t{0};
        case TypeId::DInt:
            return std::int32_t{0};
        case TypeId::Real:
            return 0.0f;
        case TypeId::LReal:
            return 0.0;
        case TypeId::Time:
            return TimeValue{0};
        case TypeId::String:
            return std::string{};
    }
    return false;
}

}  // namespace softplc::tags
