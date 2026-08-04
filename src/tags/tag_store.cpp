#include "softplc/tags/tag_store.hpp"

namespace softplc::tags {

TagId TagStore::declare(std::string name, TypeId type, Value initial,
                         std::optional<Address> addr) {
    std::unique_lock lock(mutex_);

    if (byName_.contains(name)) {
        throw std::invalid_argument("TagStore::declare: tag name already in use: " + name);
    }
    if (addr && byAddress_.contains(*addr)) {
        throw std::invalid_argument("TagStore::declare: address already in use for tag: " + name);
    }

    const TagId id = static_cast<TagId>(tags_.size());
    tags_.push_back(Tag{.name = name, .type = type, .address = addr, .value = std::move(initial)});
    byName_.emplace(std::move(name), id);
    if (addr) {
        byAddress_.emplace(*addr, id);
    }
    return id;
}

std::optional<TagId> TagStore::find(std::string_view name) const {
    std::shared_lock lock(mutex_);
    // unordered_map::find has no heterogeneous lookup for string_view pre-C++20 transparent
    // hashing setup; construct a temporary string (Phase 1: not hot-path, called at load time).
    auto it = byName_.find(std::string(name));
    if (it == byName_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<TagId> TagStore::find(Address addr) const {
    std::shared_lock lock(mutex_);
    auto it = byAddress_.find(addr);
    if (it == byAddress_.end()) {
        return std::nullopt;
    }
    return it->second;
}

Value TagStore::read(TagId id) const {
    std::shared_lock lock(mutex_);
    return at(id).value;
}

void TagStore::write(TagId id, const Value& value) {
    std::unique_lock lock(mutex_);
    if (id >= tags_.size()) {
        throw std::out_of_range("TagStore::write: invalid TagId");
    }
    tags_[id].value = value;
}

TypeId TagStore::typeOf(TagId id) const {
    std::shared_lock lock(mutex_);
    return at(id).type;
}

std::string TagStore::nameOf(TagId id) const {
    std::shared_lock lock(mutex_);
    return at(id).name;
}

std::size_t TagStore::size() const {
    std::shared_lock lock(mutex_);
    return tags_.size();
}

std::vector<Tag> TagStore::snapshot() const {
    std::shared_lock lock(mutex_);
    return tags_;
}

const Tag& TagStore::at(TagId id) const {
    if (id >= tags_.size()) {
        throw std::out_of_range("TagStore: invalid TagId");
    }
    return tags_[id];
}

}  // namespace softplc::tags
