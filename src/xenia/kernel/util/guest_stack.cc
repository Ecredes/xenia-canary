/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/util/guest_stack.h"

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/byte_order.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/object_table.h"
#include "xenia/kernel/xmodule.h"
#include "xenia/memory.h"

namespace xe {
namespace kernel {
namespace util {

std::vector<GuestModule> TitleModules(KernelState& kernel_state) {
  std::vector<GuestModule> modules;
  for (const auto& module :
       kernel_state.object_table()->GetObjectsByType<XModule>(
           XObject::Type::Module)) {
    if (module->module_type() != XModule::ModuleType::kUserModule) {
      continue;
    }
    cpu::XexModule* xex = static_cast<UserModule*>(module.get())->xex_module();
    if (!xex || !xex->base_address()) {
      continue;
    }
    GuestModule found{module->name(), xex->base_address(), xex->image_size()};
    // The table lists a module once per handle.
    if (std::none_of(modules.begin(), modules.end(),
                     [&found](const GuestModule& other) {
                       return other.base == found.base;
                     })) {
      modules.push_back(std::move(found));
    }
  }
  return modules;
}

std::vector<uint32_t> GuestStack(KernelState& kernel_state,
                                 const std::vector<GuestModule>& modules,
                                 uint32_t lr, uint32_t sp, uint32_t limit,
                                 uint32_t base, size_t max_frames) {
  Memory* memory = kernel_state.memory();
  return WalkGuestStack(
      [memory](uint32_t address) {
        return xe::load_and_swap<uint32_t>(memory->TranslateVirtual(address));
      },
      [&modules](uint32_t address) { return InModules(modules, address); }, lr,
      sp, limit, base, max_frames);
}

std::string DescribeGuestAddress(const std::vector<GuestModule>& modules,
                                 uint32_t address) {
  auto module = std::find_if(
      modules.begin(), modules.end(), [address](const GuestModule& m) {
        return address >= m.base && address - m.base < m.size;
      });
  if (module == modules.end()) {
    return fmt::format("0x{:08X}", address);
  }
  return fmt::format("{}+0x{:X}", module->name, address - module->base);
}

}  // namespace util
}  // namespace kernel
}  // namespace xe
