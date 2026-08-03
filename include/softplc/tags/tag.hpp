#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "softplc/tags/value.hpp"

namespace softplc::tags {

// IEC 61131-3 direct-addressing memory areas: %I (input), %Q (output), %M (memory).
enum class MemoryArea {
    None,
    Input,
    Output,
    Memory,
};

// A direct IEC address, e.g. %Q0.0 -> {Output, byteOffset=0, bitOffset=0}.
// bitOffset == kNoBit means the address is byte/word/dword-granular (not bit-addressed).
struct Address {
    static constexpr std::uint8_t kNoBit = 0xFF;

    MemoryArea area = MemoryArea::None;
    std::uint32_t byteOffset = 0;
    std::uint8_t bitOffset = kNoBit;

    friend bool operator==(const Address&, const Address&) = default;
    friend bool operator<(const Address& lhs, const Address& rhs) {
        if (lhs.area != rhs.area) return lhs.area < rhs.area;
        if (lhs.byteOffset != rhs.byteOffset) return lhs.byteOffset < rhs.byteOffset;
        return lhs.bitOffset < rhs.bitOffset;
    }
};

// A named variable in the tag database. Every tag has a symbolic name; some tags
// additionally carry an IEC direct address (%I/%Q/%M) usable interchangeably with the name.
struct Tag {
    std::string name;
    TypeId type = TypeId::Bool;
    std::optional<Address> address;
    Value value;
};

}  // namespace softplc::tags
