/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/host_thread_context.h"

#include <signal.h>
#include <ucontext.h>
#include <cstring>

#include "xenia/base/assert.h"

namespace xe {

void HostThreadContext::InitializeFromUcontext(const void* ucontext) {
#if XE_ARCH_AMD64
  const mcontext_t& mcontext =
      static_cast<const ucontext_t*>(ucontext)->uc_mcontext;
  rip = uint64_t(mcontext.gregs[REG_RIP]);
  eflags = uint32_t(mcontext.gregs[REG_EFL]);
  // The REG_ order may be different than the register indices in the
  // instruction encoding.
  rax = uint64_t(mcontext.gregs[REG_RAX]);
  rcx = uint64_t(mcontext.gregs[REG_RCX]);
  rdx = uint64_t(mcontext.gregs[REG_RDX]);
  rbx = uint64_t(mcontext.gregs[REG_RBX]);
  rsp = uint64_t(mcontext.gregs[REG_RSP]);
  rbp = uint64_t(mcontext.gregs[REG_RBP]);
  rsi = uint64_t(mcontext.gregs[REG_RSI]);
  rdi = uint64_t(mcontext.gregs[REG_RDI]);
  r8 = uint64_t(mcontext.gregs[REG_R8]);
  r9 = uint64_t(mcontext.gregs[REG_R9]);
  r10 = uint64_t(mcontext.gregs[REG_R10]);
  r11 = uint64_t(mcontext.gregs[REG_R11]);
  r12 = uint64_t(mcontext.gregs[REG_R12]);
  r13 = uint64_t(mcontext.gregs[REG_R13]);
  r14 = uint64_t(mcontext.gregs[REG_R14]);
  r15 = uint64_t(mcontext.gregs[REG_R15]);
  std::memcpy(xmm_registers, mcontext.fpregs->_xmm, sizeof(xmm_registers));
#elif XE_ARCH_ARM64
  const mcontext_t& mcontext =
      static_cast<const ucontext_t*>(ucontext)->uc_mcontext;
  std::memcpy(x, mcontext.regs, sizeof(x));
  sp = mcontext.sp;
  pc = mcontext.pc;
  pstate = mcontext.pstate;
  const struct fpsimd_context* mcontext_fpsimd = nullptr;
  for (auto mcontext_extension =
           reinterpret_cast<const struct _aarch64_ctx*>(mcontext.__reserved);
       mcontext_extension->magic;
       mcontext_extension = reinterpret_cast<const struct _aarch64_ctx*>(
           reinterpret_cast<const uint8_t*>(mcontext_extension) +
           mcontext_extension->size)) {
    if (mcontext_extension->magic == FPSIMD_MAGIC) {
      mcontext_fpsimd =
          reinterpret_cast<const struct fpsimd_context*>(mcontext_extension);
      break;
    }
  }
  assert_not_null(mcontext_fpsimd);
  if (mcontext_fpsimd) {
    fpsr = mcontext_fpsimd->fpsr;
    fpcr = mcontext_fpsimd->fpcr;
    std::memcpy(v, mcontext_fpsimd->vregs, sizeof(v));
  }
#endif  // XE_ARCH
}

}  // namespace xe
