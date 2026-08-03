#include "softplc/tags/tag_store.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

using namespace softplc::tags;

TEST(TagStoreTest, DeclareAndFindByName) {
    TagStore store;
    TagId id = store.declare("X", TypeId::Bool, false);
    auto found = store.find("X");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, id);
    EXPECT_EQ(store.typeOf(id), TypeId::Bool);
    EXPECT_EQ(store.nameOf(id), "X");
}

TEST(TagStoreTest, ReadWriteRoundTrip) {
    TagStore store;
    TagId id = store.declare("Counter", TypeId::DInt, std::int32_t{0});
    EXPECT_EQ(std::get<std::int32_t>(store.read(id)), 0);
    store.write(id, std::int32_t{42});
    EXPECT_EQ(std::get<std::int32_t>(store.read(id)), 42);
}

TEST(TagStoreTest, DuplicateNameThrows) {
    TagStore store;
    store.declare("X", TypeId::Bool, false);
    EXPECT_THROW(store.declare("X", TypeId::Bool, true), std::invalid_argument);
}

TEST(TagStoreTest, FindMissingNameReturnsNullopt) {
    TagStore store;
    EXPECT_FALSE(store.find("DoesNotExist").has_value());
}

TEST(TagStoreTest, AddressLookup) {
    TagStore store;
    Address addr{MemoryArea::Output, 0, 0};
    TagId id = store.declare("Led", TypeId::Bool, false, addr);
    auto found = store.find(addr);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, id);
}

TEST(TagStoreTest, DuplicateAddressThrows) {
    TagStore store;
    Address addr{MemoryArea::Output, 0, 0};
    store.declare("A", TypeId::Bool, false, addr);
    EXPECT_THROW(store.declare("B", TypeId::Bool, false, addr), std::invalid_argument);
}

TEST(TagStoreTest, InvalidTagIdThrows) {
    TagStore store;
    EXPECT_THROW(store.read(12345), std::out_of_range);
}

TEST(TagStoreTest, ForEachInAreaVisitsOnlyMatchingTags) {
    TagStore store;
    store.declare("In0", TypeId::Bool, false, Address{MemoryArea::Input, 0, 0});
    store.declare("Out0", TypeId::Bool, false, Address{MemoryArea::Output, 0, 0});
    store.declare("Mem0", TypeId::Bool, false, Address{MemoryArea::Memory, 0, 0});

    std::vector<std::string> seen;
    store.forEachInArea(MemoryArea::Output, [&](const Tag& tag) { seen.push_back(tag.name); });

    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0], "Out0");
}

TEST(TagStoreTest, ConcurrentReadersAndWriterDoNotCrash) {
    TagStore store;
    TagId id = store.declare("Shared", TypeId::DInt, std::int32_t{0});

    std::atomic<bool> stop{false};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&store, id, &stop] {
            while (!stop) {
                auto v = store.read(id);
                (void)v;
            }
        });
    }

    for (int i = 0; i < 1000; ++i) {
        store.write(id, std::int32_t{i});
    }
    stop = true;
    for (auto& t : readers) t.join();

    SUCCEED();
}
