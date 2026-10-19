/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_MEMORY_COHERENCE_H_
#define XENIA_MEMORY_COHERENCE_H_

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace xe {
namespace coherence {

// Which bytes of guest physical memory have newer data in a GPU readback
// buffer than in guest RAM, and what copying them back takes. Pure logic: the
// caller does the waiting, mapping and copying.

// A backend's GPU timeline value; 0 means nothing to wait for.
using Fence = uint64_t;

// With a pre-image (pre_buffer != 0) - what the GPU had there before the write
// - only the bytes that differ from it are copied, so a conservative range
// doesn't carry stale bytes over newer CPU writes.
struct GpuWrite {
  uint32_t physical_address;
  uint32_t length;
  uint64_t buffer;
  uint32_t buffer_offset;
  Fence fence;
  uint64_t pre_buffer = 0;
  uint32_t pre_offset = 0;
};

struct Copy {
  uint64_t buffer;
  uint32_t buffer_offset;
  uint32_t physical_address;
  uint32_t length;
  Fence fence;
  uint64_t pre_buffer = 0;
  uint32_t pre_offset = 0;
};
bool operator==(const Copy& a, const Copy& b);

enum class Access : uint8_t { kNoAccess, kReadOnly, kReadWrite };

// One guest view of a system page.
struct PageView {
  Access guest;
  bool committed;
  bool write_watched;
};

// The host protection of a view of a page. A view the guest didn't commit the
// page in is still mapped read-write on the host.
Access EffectiveHostAccess(const PageView& view, bool gpu_owned);

struct Stats {
  uint64_t writes_recorded = 0;
  uint64_t bytes_superseded = 0;
  uint64_t pages_pulled = 0;
  uint64_t bytes_pulled = 0;
  uint64_t bytes_discarded = 0;
  uint64_t buffer_flushes = 0;
};

class Core {
 public:
  static constexpr uint32_t kPageShift = 12;

  // The bytes become GPU-owned; older pending bytes they overlap are dropped.
  void RecordGpuWrite(const GpuWrite& write);
  // The newest fence of pending bytes on the pages the range touches.
  Fence FenceFor(uint32_t physical_address, uint32_t length) const;
  // The newest fence of pending bytes from a buffer.
  Fence FenceForBuffer(uint64_t buffer) const;
  // Removes and returns the pending bytes on the pages the range touches,
  // oldest first.
  std::vector<Copy> TakeCopies(uint32_t physical_address, uint32_t length);
  // Removes and returns everything still pending from a buffer.
  std::vector<Copy> TakeCopiesFromBuffer(uint64_t buffer);
  // Drops pending bytes in the range, returning what it dropped.
  std::vector<Copy> Discard(uint32_t physical_address, uint32_t length);
  bool IsGpuOwned(uint32_t physical_page) const;
  const Stats& stats() const { return stats_; }

 private:
  struct Piece {
    uint32_t start;
    uint32_t end;
    uint64_t buffer;
    uint32_t buffer_offset;
    Fence fence;
    uint64_t pre_buffer;
    uint32_t pre_offset;
  };

  void CountPages(const Piece& piece, int32_t delta);
  // Removes [start, end) from every piece, returning the bytes removed and,
  // if copies isn't null, appending the removed bytes as copies.
  uint64_t Subtract(uint32_t start, uint32_t end, std::vector<Copy>* copies);

  // Recording order, oldest first.
  std::vector<Piece> pieces_;
  // Pending pieces touching each page.
  std::unordered_map<uint32_t, uint32_t> page_piece_counts_;
  Stats stats_;
};

}  // namespace coherence
}  // namespace xe

#endif  // XENIA_MEMORY_COHERENCE_H_
