/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/memory_coherence.h"

#include <algorithm>
#include <numeric>

namespace xe {
namespace coherence {

bool operator==(const Copy& a, const Copy& b) {
  return a.buffer == b.buffer && a.buffer_offset == b.buffer_offset &&
         a.physical_address == b.physical_address && a.length == b.length &&
         a.fence == b.fence && a.pre_buffer == b.pre_buffer &&
         a.pre_offset == b.pre_offset;
}

Access EffectiveHostAccess(const PageView& view, bool gpu_owned) {
  if (gpu_owned) {
    return Access::kNoAccess;
  }
  if (!view.committed) {
    return view.write_watched ? Access::kReadOnly : Access::kReadWrite;
  }
  if (view.guest == Access::kReadWrite && view.write_watched) {
    return Access::kReadOnly;
  }
  return view.guest;
}

void Core::CountPages(const Piece& piece, int32_t delta) {
  uint32_t last = (piece.end - 1) >> kPageShift;
  for (uint32_t page = piece.start >> kPageShift; page <= last; ++page) {
    uint32_t& count = page_piece_counts_[page];
    count += delta;
    if (!count) {
      page_piece_counts_.erase(page);
    }
  }
}

uint64_t Core::Subtract(uint32_t start, uint32_t end,
                        std::vector<Copy>* copies) {
  uint64_t removed = 0;
  std::vector<Piece> kept;
  kept.reserve(pieces_.size() + 1);
  for (const Piece& piece : pieces_) {
    uint32_t cut_start = std::max(piece.start, start);
    uint32_t cut_end = std::min(piece.end, end);
    if (cut_start >= cut_end) {
      kept.push_back(piece);
      continue;
    }
    removed += cut_end - cut_start;
    if (copies) {
      copies->push_back(
          {piece.buffer, piece.buffer_offset + (cut_start - piece.start),
           cut_start, cut_end - cut_start, piece.fence, piece.pre_buffer,
           piece.pre_buffer ? piece.pre_offset + (cut_start - piece.start)
                            : 0});
    }
    CountPages(piece, -1);
    if (piece.start < cut_start) {
      Piece left = piece;
      left.end = cut_start;
      CountPages(left, 1);
      kept.push_back(left);
    }
    if (cut_end < piece.end) {
      Piece right = piece;
      right.buffer_offset += cut_end - piece.start;
      if (right.pre_buffer) {
        right.pre_offset += cut_end - piece.start;
      }
      right.start = cut_end;
      CountPages(right, 1);
      kept.push_back(right);
    }
  }
  pieces_ = std::move(kept);
  return removed;
}

void Core::RecordGpuWrite(const GpuWrite& write) {
  if (!write.length) {
    return;
  }
  uint32_t end = write.physical_address + write.length;
  stats_.bytes_superseded += Subtract(write.physical_address, end, nullptr);
  Piece piece{write.physical_address, end,         write.buffer,
              write.buffer_offset,    write.fence, write.pre_buffer,
              write.pre_offset};
  CountPages(piece, 1);
  pieces_.push_back(piece);
  ++stats_.writes_recorded;
}

Fence Core::FenceFor(uint32_t physical_address, uint32_t length) const {
  if (!length) {
    return 0;
  }
  uint32_t start = physical_address & ~((uint32_t(1) << kPageShift) - 1);
  uint64_t end = (((uint64_t(physical_address) + length - 1) >> kPageShift) + 1)
                 << kPageShift;
  return std::accumulate(pieces_.begin(), pieces_.end(), Fence(0),
                         [&](Fence fence, const Piece& piece) {
                           return piece.start < end && start < piece.end
                                      ? std::max(fence, piece.fence)
                                      : fence;
                         });
}

Fence Core::FenceForBuffer(uint64_t buffer) const {
  return std::accumulate(
      pieces_.begin(), pieces_.end(), Fence(0),
      [&](Fence fence, const Piece& piece) {
        return piece.buffer == buffer ? std::max(fence, piece.fence) : fence;
      });
}

std::vector<Copy> Core::TakeCopies(uint32_t physical_address, uint32_t length) {
  std::vector<Copy> copies;
  if (!length) {
    return copies;
  }
  uint32_t page_first = physical_address >> kPageShift;
  uint32_t page_last =
      uint32_t((uint64_t(physical_address) + length - 1) >> kPageShift);
  for (uint32_t page = page_first; page <= page_last; ++page) {
    if (IsGpuOwned(page)) {
      ++stats_.pages_pulled;
    }
  }
  uint64_t end = uint64_t(page_last + 1) << kPageShift;
  stats_.bytes_pulled +=
      Subtract(page_first << kPageShift,
               uint32_t(std::min<uint64_t>(end, UINT32_MAX)), &copies);
  return copies;
}

std::vector<Copy> Core::TakeCopiesFromBuffer(uint64_t buffer) {
  std::vector<Copy> copies;
  std::vector<Piece> kept;
  for (const Piece& piece : pieces_) {
    if (piece.buffer != buffer) {
      kept.push_back(piece);
      continue;
    }
    copies.push_back({piece.buffer, piece.buffer_offset, piece.start,
                      piece.end - piece.start, piece.fence, piece.pre_buffer,
                      piece.pre_offset});
    CountPages(piece, -1);
  }
  pieces_ = std::move(kept);
  if (!copies.empty()) {
    ++stats_.buffer_flushes;
  }
  return copies;
}

std::vector<Copy> Core::Discard(uint32_t physical_address, uint32_t length) {
  std::vector<Copy> dropped;
  if (!length) {
    return dropped;
  }
  uint64_t end =
      std::min<uint64_t>(uint64_t(physical_address) + length, UINT32_MAX);
  stats_.bytes_discarded += Subtract(physical_address, uint32_t(end), &dropped);
  return dropped;
}

bool Core::IsGpuOwned(uint32_t physical_page) const {
  return page_piece_counts_.count(physical_page) != 0;
}

}  // namespace coherence
}  // namespace xe
