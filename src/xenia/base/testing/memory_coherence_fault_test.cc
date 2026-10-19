/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <memory>
#include <utility>
#include <vector>

#include "third_party/catch/include/catch.hpp"

#include "xenia/memory.h"

namespace xe {
namespace test {

namespace {

struct InvalidationLog {
  std::vector<std::pair<uint32_t, uint32_t>> ranges;

  static std::pair<uint32_t, uint32_t> Callback(void* context,
                                                uint32_t physical_start,
                                                uint32_t length,
                                                bool exact_range) {
    static_cast<InvalidationLog*>(context)->ranges.emplace_back(physical_start,
                                                                length);
    return {0, UINT32_MAX};
  }

  bool Covered(uint32_t physical_address) const {
    for (const auto& range : ranges) {
      if (physical_address >= range.first &&
          physical_address - range.first < range.second) {
        return true;
      }
    }
    return false;
  }
};

}  // namespace

// The whole fault path is one test case: Catch swaps the access violation
// handler per test case run and restores the previous one afterwards, which
// would remove the one Memory installs.
TEST_CASE("Memory coherence: fault path", "[memory_coherence]") {
  // Local, so its views at 0x100000000 are unmapped before later tests map
  // views there.
  auto memory_owner = std::make_unique<Memory>();
  Memory& memory = *memory_owner;
  REQUIRE(memory.Initialize());

  // host_write_notifies: a declared host write notifies the invalidation
  // callbacks of the pages it covers.
  {
    uint32_t address;
    REQUIRE(memory.LookupHeap(0xA0000000)
                ->Alloc(0x10000, 0x10000,
                        kMemoryAllocationReserve | kMemoryAllocationCommit,
                        kMemoryProtectRead | kMemoryProtectWrite, false,
                        &address));
    uint32_t physical = memory.GetPhysicalAddress(address);
    InvalidationLog log;
    void* handle = memory.RegisterPhysicalMemoryInvalidationCallback(
        InvalidationLog::Callback, &log);
    memory.EnablePhysicalMemoryAccessCallbacks(physical, 0x1000, true, false);
    memory.FinishHostWrite(physical + 0x10, 4);
    memory.UnregisterPhysicalMemoryInvalidationCallback(handle);
    REQUIRE(log.Covered(physical + 0x10));
  }

  // host_buffer_bypasses_watch: a buffer for a host API resolves to the
  // physical mapping, so a watched page doesn't fault, and the finished write
  // notifies.
  {
    uint32_t address;
    REQUIRE(memory.LookupHeap(0xA0000000)
                ->Alloc(0x10000, 0x10000,
                        kMemoryAllocationReserve | kMemoryAllocationCommit,
                        kMemoryProtectRead | kMemoryProtectWrite, false,
                        &address));
    uint32_t physical = memory.GetPhysicalAddress(address);
    InvalidationLog log;
    void* handle = memory.RegisterPhysicalMemoryInvalidationCallback(
        InvalidationLog::Callback, &log);
    memory.EnablePhysicalMemoryAccessCallbacks(physical, 0x1000, true, false);
    uint8_t* buffer = memory.PrepareHostBuffer(address + 0x20, 4, true);
    REQUIRE(buffer == memory.TranslatePhysical(physical + 0x20));
    buffer[0] = 0x5A;
    REQUIRE(log.ranges.empty());
    memory.FinishHostBufferWrite(address + 0x20, 4);
    memory.UnregisterPhysicalMemoryInvalidationCallback(handle);
    REQUIRE(log.Covered(physical + 0x20));
    REQUIRE(*memory.TranslateVirtual(address + 0x20) == 0x5A);
  }

  // host_buffer_virtual: buffers outside the physical views translate
  // virtually, and a null buffer stays null.
  {
    uint32_t address;
    REQUIRE(memory.LookupHeap(0x40000000)
                ->Alloc(0x1000, 0x1000,
                        kMemoryAllocationReserve | kMemoryAllocationCommit,
                        kMemoryProtectRead | kMemoryProtectWrite, false,
                        &address));
    REQUIRE(memory.PrepareHostBuffer(address, 4, false) ==
            memory.TranslateVirtual(address));
    REQUIRE(memory.PrepareHostBuffer(0, 4, false) == nullptr);
  }
}

}  // namespace test
}  // namespace xe
