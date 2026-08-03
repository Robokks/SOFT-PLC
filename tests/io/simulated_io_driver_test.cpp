#include "softplc/io/simulated_io_driver.hpp"

#include <gtest/gtest.h>

#include "softplc/tags/tag_store.hpp"

using namespace softplc;

TEST(SimulatedIoDriverTest, ReadInputsAppliesSimulatedValue) {
    tags::TagStore tags;
    tags::Address addr{tags::MemoryArea::Input, 0, 0};
    tags::TagId id = tags.declare("In0", tags::TypeId::Bool, false, addr);

    io::SimulatedIoDriver driver;
    driver.setSimulatedInput(addr, true);
    driver.readInputs(tags);

    EXPECT_EQ(std::get<bool>(tags.read(id)), true);
}

TEST(SimulatedIoDriverTest, WriteOutputsRecordsTagValue) {
    tags::TagStore tags;
    tags::Address addr{tags::MemoryArea::Output, 0, 0};
    tags::TagId id = tags.declare("Out0", tags::TypeId::Bool, false, addr);
    tags.write(id, true);

    io::SimulatedIoDriver driver;
    driver.writeOutputs(tags);

    auto last = driver.lastOutput(addr);
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(std::get<bool>(*last), true);
}

TEST(SimulatedIoDriverTest, LastOutputUnsetReturnsNullopt) {
    io::SimulatedIoDriver driver;
    tags::Address addr{tags::MemoryArea::Output, 5, 0};
    EXPECT_FALSE(driver.lastOutput(addr).has_value());
}
