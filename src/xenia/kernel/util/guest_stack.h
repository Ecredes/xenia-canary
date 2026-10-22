/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_UTIL_GUEST_STACK_H_
#define XENIA_KERNEL_UTIL_GUEST_STACK_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xe {
namespace kernel {
class KernelState;
namespace util {

// A loaded title module (the executable or one of its DLLs): where its image
// is, for reading return addresses as module and offset.
struct GuestModule {
  std::string name;
  uint32_t base = 0;
  uint32_t size = 0;
};

inline bool InModules(const std::vector<GuestModule>& modules,
                      uint32_t address) {
  return address % 4 == 0 &&
         std::any_of(modules.begin(), modules.end(),
                     [address](const GuestModule& module) {
                       return address >= module.base &&
                              address - module.base < module.size;
                     });
}

// Return addresses on a guest stack, innermost first: the first is lr, the
// return address of the call the thread is in; then the saved return address
// of each frame up the back chain. Xbox 360 code saves a function's return
// address at -8 from the stack pointer it was called with, which is the back
// chain word of its own frame. The walk stays within [limit, base), the
// thread's stack, and ends at a chain that doesn't climb or a return address
// that is_code refuses, outside the title's modules (past the outermost frame,
// an unused stack holds fill bytes).
template <typename Read32, typename IsCode>
std::vector<uint32_t> WalkGuestStack(Read32 read32, IsCode is_code, uint32_t lr,
                                     uint32_t sp, uint32_t limit, uint32_t base,
                                     size_t max_frames) {
  std::vector<uint32_t> stack;
  if (!is_code(lr)) {
    return stack;
  }
  stack.push_back(lr);
  while (stack.size() < max_frames && sp >= limit && sp < base && sp % 4 == 0) {
    uint32_t back = read32(sp);
    if (back <= sp || back > base || back - 8 < limit) {
      break;
    }
    uint32_t saved_lr = read32(back - 8);
    if (!is_code(saved_lr)) {
      break;
    }
    stack.push_back(saved_lr);
    sp = back;
  }
  return stack;
}

// The title's modules now. Guest code is in them alone; Xenia's system
// modules are native.
std::vector<GuestModule> TitleModules(KernelState& kernel_state);

// WalkGuestStack over guest memory, within the title's modules.
std::vector<uint32_t> GuestStack(KernelState& kernel_state,
                                 const std::vector<GuestModule>& modules,
                                 uint32_t lr, uint32_t sp, uint32_t limit,
                                 uint32_t base, size_t max_frames = 16);

// An address as module+offset when it falls in a module, else hex.
std::string DescribeGuestAddress(const std::vector<GuestModule>& modules,
                                 uint32_t address);

}  // namespace util
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_UTIL_GUEST_STACK_H_
