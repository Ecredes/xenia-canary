/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "third_party/catch/include/catch.hpp"

#include "xenia/base/mutex.h"
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

constexpr uint32_t kGpuWord = 0x47474747;
constexpr uint32_t kStaleWord = 0x5A5A5A5A;

// A GPU whose fences pass when the test says so.
class FakeSource : public GpuPullSource {
 public:
  void AwaitFence(coherence::Fence fence) override {
    ++awaits;
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [&] { return passed_ >= fence || shut_down_; });
  }
  const uint8_t* MapForRead(uint64_t buffer) override {
    return MapBuffer(buffer);
  }
  void Unmap(uint64_t buffer) override {}
  // Buffer 1 is all 0x47; others are what the test put there.
  const uint8_t* MapBuffer(uint64_t buffer) {
    return buffer == 1 ? data_.data() : other_[buffer].data();
  }
  void SetBuffer(uint64_t buffer, std::vector<uint8_t> data) {
    other_[buffer] = std::move(data);
  }

  void Pass(coherence::Fence fence) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      passed_ = fence;
    }
    cv_.notify_all();
  }
  void ShutDown() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      shut_down_ = true;
    }
    cv_.notify_all();
  }

  std::atomic<int> awaits{0};

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  coherence::Fence passed_ = 0;
  bool shut_down_ = false;
  std::vector<uint8_t> data_ = std::vector<uint8_t>(0x10000, 0x47);
  std::map<uint64_t, std::vector<uint8_t>> other_;
};

// A 64 KB allocation through 0xE0000000 and its words in three views.
struct Views {
  uint32_t address = 0;
  uint32_t physical = 0;
  volatile uint32_t* a = nullptr;
  volatile uint32_t* c = nullptr;
  volatile uint32_t* e = nullptr;
};

Views AllocateViews(Memory& memory) {
  Views views;
  REQUIRE(memory.LookupHeap(0xE0000000)
              ->Alloc(0x10000, 0x1000,
                      kMemoryAllocationReserve | kMemoryAllocationCommit,
                      kMemoryProtectRead | kMemoryProtectWrite, false,
                      &views.address));
  views.physical = memory.GetPhysicalAddress(views.address);
  auto* stale = memory.TranslatePhysical<uint32_t*>(views.physical);
  for (uint32_t i = 0; i < 0x10000 / 4; ++i) {
    stale[i] = kStaleWord;
  }
  views.a =
      memory.TranslateVirtual<volatile uint32_t*>(0xA0000000 + views.physical);
  views.c =
      memory.TranslateVirtual<volatile uint32_t*>(0xC0000000 + views.physical);
  views.e = memory.TranslateVirtual<volatile uint32_t*>(views.address);
  return views;
}

template <typename T>
bool StillBlocked(std::future<T>& future) {
  return future.wait_for(std::chrono::milliseconds(200)) ==
         std::future_status::timeout;
}

std::future<uint32_t> ReadAsync(volatile uint32_t* word) {
  return std::async(std::launch::async, [word] { return uint32_t(*word); });
}

std::future<void> WriteAsync(volatile uint32_t* word, uint32_t value) {
  return std::async(std::launch::async, [word, value] { *word = value; });
}

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

  FakeSource source;
  memory.SetGpuPullSource(&source);

  // host_buffer_pulls: a buffer for a host API (a system call, which never
  // reaches the fault handler) is pulled before the host accesses it.
  {
    Views views = AllocateViews(memory);
    memory.RecordGpuWrite({views.physical, 0x1000, 1, 0, 2});
    source.Pass(2);
    auto* buffer = reinterpret_cast<uint32_t*>(
        memory.PrepareHostBuffer(views.address + 4, 8, false));
    REQUIRE(buffer[0] == kGpuWord);
    REQUIRE(buffer[1] == kGpuWord);
  }

  // pull_through_other_view: written by the GPU, read through a view the
  // guest didn't allocate it through.
  {
    Views views = AllocateViews(memory);
    memory.RecordGpuWrite({views.physical, 0x1000, 1, 0, 3});
    auto reader = ReadAsync(views.c);
    REQUIRE(StillBlocked(reader));
    // The fault waits without the global lock.
    auto locker = std::async(std::launch::async, [] {
      auto lock = xe::global_critical_region::Acquire();
    });
    REQUIRE(locker.wait_for(std::chrono::seconds(2)) ==
            std::future_status::ready);
    source.Pass(3);
    REQUIRE(reader.get() == kGpuWord);
  }

  // two_readers_one_copy
  {
    Views views = AllocateViews(memory);
    uint64_t pulled = memory.coherence_stats().pages_pulled;
    memory.RecordGpuWrite({views.physical, 0x1000, 1, 0, 4});
    auto first = ReadAsync(views.c);
    auto second = ReadAsync(views.a + 1);
    REQUIRE(StillBlocked(first));
    REQUIRE(StillBlocked(second));
    source.Pass(4);
    REQUIRE(first.get() == kGpuWord);
    REQUIRE(second.get() == kGpuWord);
    REQUIRE(memory.coherence_stats().pages_pulled == pulled + 1);
  }

  // write_during_pull_merges
  {
    Views views = AllocateViews(memory);
    memory.RecordGpuWrite({views.physical, 0x1000, 1, 0, 5});
    auto reader = ReadAsync(views.c + 1);
    auto writer = WriteAsync(views.a, 0x00C0FFEE);
    REQUIRE(StillBlocked(reader));
    REQUIRE(StillBlocked(writer));
    source.Pass(5);
    REQUIRE(reader.get() == kGpuWord);
    writer.get();
    auto* words = memory.TranslatePhysical<uint32_t*>(views.physical);
    REQUIRE(words[0] == 0x00C0FFEE);
    REQUIRE(words[1] == kGpuWord);
    REQUIRE(words[2] == kGpuWord);
  }

  // at_most_once_across_views
  {
    Views views = AllocateViews(memory);
    memory.RecordGpuWrite({views.physical, 0x1000, 1, 0, 6});
    source.Pass(6);
    REQUIRE(uint32_t(*views.c) == kGpuWord);
    *views.a = 0x11111111;
    REQUIRE(uint32_t(*views.e) == 0x11111111);
  }

  // free_discards: released memory drops its pending GPU data unawaited.
  {
    Views views = AllocateViews(memory);
    uint64_t discarded = memory.coherence_stats().bytes_discarded;
    int awaits = source.awaits;
    memory.RecordGpuWrite({views.physical, 0x1000, 1, 0, 100});
    REQUIRE(memory.LookupHeap(0xE0000000)->Release(views.address));
    REQUIRE(memory.coherence_stats().bytes_discarded == discarded + 0x1000);
    auto reader = ReadAsync(views.a);
    REQUIRE_FALSE(StillBlocked(reader));
    reader.get();
    REQUIRE(source.awaits == awaits);
  }

  // host_write_merges: a declared host write lands next to GPU data.
  {
    Views views = AllocateViews(memory);
    memory.RecordGpuWrite({views.physical, 0x1000, 1, 0, 7});
    source.Pass(7);
    memory.PrepareHostWrite(views.physical + 0x100, 4);
    *memory.TranslatePhysical<uint32_t*>(views.physical + 0x100) = 0x22222222;
    memory.FinishHostWrite(views.physical + 0x100, 4);
    REQUIRE(uint32_t(views.c[0x100 / 4]) == 0x22222222);
    REQUIRE(uint32_t(views.c[0]) == kGpuWord);
  }

  // write_watch_committed_views_only: CPU write watches stay in the views the
  // guest committed the page in. Until every GPU write is registered, a write
  // through another view must not drop GPU-only data from the GPU copy.
  {
    Views views = AllocateViews(memory);
    InvalidationLog log;
    void* handle = memory.RegisterPhysicalMemoryInvalidationCallback(
        InvalidationLog::Callback, &log);
    memory.EnablePhysicalMemoryAccessCallbacks(views.physical, 0x1000, true,
                                               false);
    *views.a = 0x33333333;
    bool through_other_view = log.Covered(views.physical);
    *views.e = 0x44444444;
    memory.UnregisterPhysicalMemoryInvalidationCallback(handle);
    REQUIRE_FALSE(through_other_view);
    REQUIRE(log.Covered(views.physical));
  }

  // delta_keeps_cpu_bytes: with a pre-image, a pull writes only the bytes the
  // GPU changed, so a CPU write made after the pre-image survives.
  {
    Views views = AllocateViews(memory);
    // The pre-image is what the GPU had before the draw; the shader then
    // exported word 1 only.
    source.SetBuffer(2, std::vector<uint8_t>(8, 0x5A));
    std::vector<uint8_t> post(8, 0x5A);
    std::fill(post.begin() + 4, post.end(), uint8_t(0x47));
    source.SetBuffer(3, post);
    auto* guest = memory.TranslatePhysical<uint32_t*>(views.physical);
    guest[0] = 0x11111111;  // A CPU write after the pre-image was taken.
    memory.RecordGpuWrite({views.physical, 8, 3, 0, 8, 2, 0});
    source.Pass(8);
    REQUIRE(uint32_t(views.c[0]) == 0x11111111);
    REQUIRE(uint32_t(views.c[1]) == kGpuWord);
  }

  // device_writes_break_reservations: host writes and pulls report the ranges
  // they wrote, so reservations on them can be broken.
  {
    Views views = AllocateViews(memory);
    InvalidationLog reserved;
    memory.SetReservationInvalidator(
        [](void* context, uint32_t physical_address, uint32_t length) {
          InvalidationLog::Callback(context, physical_address, length, true);
        },
        &reserved);
    memory.FinishHostWrite(views.physical + 0x20, 4);
    REQUIRE(reserved.Covered(views.physical + 0x20));
    memory.RecordGpuWrite({views.physical + 0x2000, 0x1000, 1, 0, 9});
    source.Pass(9);
    REQUIRE(uint32_t(views.c[0x2000 / 4]) == kGpuWord);
    memory.SetReservationInvalidator(nullptr, nullptr);
    REQUIRE(reserved.Covered(views.physical + 0x2000));
  }

  // flush_waits_unlocked: flushing a buffer before its reuse waits for the GPU
  // without the global lock.
  {
    Views views = AllocateViews(memory);
    memory.RecordGpuWrite({views.physical, 0x1000, 1, 0, 13});
    auto flusher =
        std::async(std::launch::async, [&] { memory.FlushGpuBuffer(1); });
    REQUIRE(StillBlocked(flusher));
    auto locker = std::async(std::launch::async, [] {
      auto lock = xe::global_critical_region::Acquire();
    });
    REQUIRE(locker.wait_for(std::chrono::seconds(2)) ==
            std::future_status::ready);
    source.Pass(13);
    flusher.get();
    REQUIRE(*memory.TranslatePhysical<uint32_t*>(views.physical) == kGpuWord);
  }

  // source_removal_waits_for_pulls: removing the source returns only once no
  // pull still waits on it, and the waiting reader then gets guest RAM.
  {
    Views views = AllocateViews(memory);
    memory.RecordGpuWrite({views.physical, 0x1000, 1, 0, 1000});
    auto reader = ReadAsync(views.c);
    REQUIRE(StillBlocked(reader));
    auto remover = std::async(std::launch::async,
                              [&] { memory.SetGpuPullSource(nullptr); });
    REQUIRE(StillBlocked(remover));
    source.ShutDown();
    remover.get();
    REQUIRE(reader.get() == kStaleWord);
  }
}

}  // namespace test
}  // namespace xe
