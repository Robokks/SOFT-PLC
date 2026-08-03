#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "softplc/tags/tag.hpp"
#include "softplc/tags/value.hpp"

namespace softplc::tags {

using TagId = std::uint32_t;
inline constexpr TagId kInvalidTagId = static_cast<TagId>(-1);

// Thread-safe tag/variable database. Multiple concurrent readers (e.g. future
// OPC-UA/Modbus/HMI clients) are supported via a shared_mutex; the scan thread and
// any declare() calls take the exclusive lock.
//
// Hot-path read()/write() are intended to be called with a TagId resolved once at
// program-load time (e.g. by the ST interpreter binder), not by repeated name lookup.
class TagStore {
public:
    // Declares a new tag. Throws std::invalid_argument if the name is already in use
    // or if addr is already bound to another tag.
    TagId declare(std::string name, TypeId type, Value initial,
                   std::optional<Address> addr = std::nullopt);

    [[nodiscard]] std::optional<TagId> find(std::string_view name) const;
    [[nodiscard]] std::optional<TagId> find(Address addr) const;

    [[nodiscard]] Value read(TagId id) const;
    void write(TagId id, const Value& value);

    [[nodiscard]] TypeId typeOf(TagId id) const;
    [[nodiscard]] std::string nameOf(TagId id) const;

    [[nodiscard]] std::size_t size() const;

    // Invokes fn(const Tag&) for every declared tag whose address falls in `area`,
    // while holding a single shared lock for the whole scan (used by I/O drivers).
    template <typename F>
    void forEachInArea(MemoryArea area, F&& fn) const {
        std::shared_lock lock(mutex_);
        for (const auto& tag : tags_) {
            if (tag.address && tag.address->area == area) {
                fn(tag);
            }
        }
    }

    // Applies fn(Tag&) to the tag at `id` while holding the exclusive lock; used by
    // I/O drivers to update input tags in place without a read/write round trip.
    template <typename F>
    void updateInArea(MemoryArea area, F&& fn) {
        std::unique_lock lock(mutex_);
        for (auto& tag : tags_) {
            if (tag.address && tag.address->area == area) {
                fn(tag);
            }
        }
    }

private:
    [[nodiscard]] const Tag& at(TagId id) const;

    mutable std::shared_mutex mutex_;
    std::vector<Tag> tags_;
    std::unordered_map<std::string, TagId> byName_;
    std::map<Address, TagId> byAddress_;
};

}  // namespace softplc::tags
