#include "softplc/tags/value.hpp"

#include <iomanip>
#include <sstream>
#include <type_traits>

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

std::string jsonEscapeString(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (const char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    std::ostringstream hex;
                    hex << "\\u" << std::hex << std::setfill('0') << std::setw(4)
                        << static_cast<int>(c);
                    out += hex.str();
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
    return out;
}

std::string toJson(const Value& value) {
    return std::visit(
        [](auto&& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>) {
                return v ? "true" : "false";
            } else if constexpr (std::is_same_v<T, TimeValue>) {
                return std::to_string(v.count());
            } else if constexpr (std::is_same_v<T, std::string>) {
                return jsonEscapeString(v);
            } else {
                return std::to_string(v);
            }
        },
        value);
}

}  // namespace softplc::tags
