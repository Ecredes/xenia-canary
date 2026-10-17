/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/stack_walker.h"

#include <dlfcn.h>
#include <unwind.h>

#include <algorithm>
#include <cstring>

#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/string_util.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/backend/code_cache.h"

namespace xe {
namespace cpu {

namespace {

// Enough for the frames requested plus the signal handler frames above the
// interrupted one.
constexpr size_t kMaxCapturedFrames = 128;

struct BacktraceState {
  uint64_t* pcs;
  size_t capacity;
  size_t count;
};

_Unwind_Reason_Code BacktraceCallback(_Unwind_Context* context, void* data) {
  auto state = static_cast<BacktraceState*>(data);
  if (state->count >= state->capacity) {
    return _URC_END_OF_STACK;
  }
  int ip_before_insn = 0;
  uintptr_t pc = _Unwind_GetIPInfo(context, &ip_before_insn);
  if (!pc) {
    return _URC_END_OF_STACK;
  }
  state->pcs[state->count++] = pc;
  return _URC_NO_REASON;
}

// Captures the calling thread's stack, innermost frame first. The JIT
// registers .eh_frame unwind info for its code, so this also walks through
// guest functions and thunks.
size_t Backtrace(uint64_t* pcs, size_t capacity) {
  BacktraceState state = {pcs, capacity, 0};
  _Unwind_Backtrace(BacktraceCallback, &state);
  return state.count;
}

uint64_t GetContextPc(const HostThreadContext& context) {
#if XE_ARCH_AMD64
  return context.rip;
#elif XE_ARCH_ARM64
  return context.pc;
#else
  return 0;
#endif
}

// Filled in on the suspended thread by CaptureSuspendedThread.
struct SuspendedCapture {
  const backend::CodeCache* code_cache;
  bool unwinder_busy;
  size_t count;
  uint64_t pcs[kMaxCapturedFrames];
  bool has_context;
  HostThreadContext context;
};

// Runs on the suspended thread, inside its suspend signal handler.
void CaptureSuspendedThread(void* data, const void* interrupted_context) {
  auto capture = static_cast<SuspendedCapture*>(data);
  if (interrupted_context) {
    capture->context.InitializeFromUcontext(interrupted_context);
    capture->has_context = true;
  }
  // The unwinder's lookup spins while registration holds its lock. If this
  // thread was interrupted inside registration, it never would get the lock.
  if (capture->code_cache->IsRegisteringUnwindInfo()) {
    capture->unwinder_busy = true;
    return;
  }
  capture->count = Backtrace(capture->pcs, kMaxCapturedFrames);
}

uint64_t HashStack(const uint64_t* pcs, size_t count) {
  // FNV-1a.
  uint64_t hash = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < count; ++i) {
    hash = (hash ^ pcs[i]) * 0x100000001B3ull;
  }
  return hash;
}

}  // namespace

class PosixStackWalker : public StackWalker {
 public:
  explicit PosixStackWalker(backend::CodeCache* code_cache)
      : code_cache_(code_cache),
        code_cache_min_(code_cache->execute_base_address()),
        code_cache_max_(code_cache_min_ + code_cache->total_size()) {}

  size_t CaptureStackTrace(uint64_t* frame_host_pcs, size_t frame_offset,
                           size_t frame_count,
                           uint64_t* out_stack_hash) override {
    uint64_t pcs[kMaxCapturedFrames];
    size_t count = Backtrace(pcs, kMaxCapturedFrames);
    // Skip this function, like CaptureStackBackTrace on Windows.
    return CopyFrames(pcs, count, frame_offset + 1, frame_host_pcs, frame_count,
                      out_stack_hash);
  }

  size_t CaptureStackTrace(threading::Thread* thread, uint64_t* frame_host_pcs,
                           size_t frame_offset, size_t frame_count,
                           const HostThreadContext* in_host_context,
                           HostThreadContext* out_host_context,
                           uint64_t* out_stack_hash) override {
    if (out_stack_hash) {
      *out_stack_hash = 0;
    }

    auto capture = std::make_unique<SuspendedCapture>();
    capture->code_cache = code_cache_;
    bool is_current_thread = thread == threading::Thread::GetCurrentThread();
    if (is_current_thread) {
      // For example a breakpoint handler walking its own thread, which passes
      // the context of the exception in in_host_context.
      capture->count = Backtrace(capture->pcs, kMaxCapturedFrames);
    } else if (!threading::RunOnSuspendedThread(thread, CaptureSuspendedThread,
                                                capture.get())) {
      XELOGE("Unable to capture the stack of a thread that isn't suspended");
      return 0;
    }
    if (capture->unwinder_busy) {
      XELOGW("Not walking a stack while JIT unwind info is being registered");
      return 0;
    }

    const HostThreadContext* context = in_host_context;
    if (!context && capture->has_context) {
      context = &capture->context;
    }
    if (out_host_context && context) {
      *out_host_context = *context;
    }

    // The walk started in the signal handler (or in this function). Drop the
    // frames above the one the thread was stopped in.
    size_t first_frame = is_current_thread ? 1 : 0;
    if (context) {
      first_frame = 0;
      uint64_t pc = GetContextPc(*context);
      while (first_frame < capture->count && capture->pcs[first_frame] != pc) {
        ++first_frame;
      }
      if (first_frame == capture->count) {
        XELOGE("Stack walk didn't reach the interrupted frame at {:016X}", pc);
        return 0;
      }
    }
    return CopyFrames(capture->pcs, capture->count, first_frame + frame_offset,
                      frame_host_pcs, frame_count, out_stack_hash);
  }

  bool ResolveStack(uint64_t* frame_host_pcs, StackFrame* frames,
                    size_t frame_count) override {
    for (size_t i = 0; i < frame_count; ++i) {
      auto& frame = frames[i];
      std::memset(&frame, 0, sizeof(frame));
      frame.host_pc = frame_host_pcs[i];

      if (frame.host_pc >= code_cache_min_ && frame.host_pc < code_cache_max_) {
        // Generated code, so the code cache knows it.
        frame.type = StackFrame::Type::kGuest;
        auto function = code_cache_->LookupFunction(frame.host_pc);
        frame.guest_symbol.function = function;
        if (function && function->is_guest()) {
          frame.guest_pc = static_cast<GuestFunction*>(function)
                               ->MapMachineCodeToGuestAddress(frame.host_pc);
        }
      } else {
        // Emulator or system code. Only exported symbols have names here.
        frame.type = StackFrame::Type::kHost;
        Dl_info info;
        if (dladdr(reinterpret_cast<void*>(frame.host_pc), &info) &&
            info.dli_sname) {
          frame.host_symbol.address = uint64_t(info.dli_saddr);
          string_util::copy_truncating(frame.host_symbol.name, info.dli_sname,
                                       xe::countof(frame.host_symbol.name));
        }
      }
    }
    return true;
  }

 private:
  static size_t CopyFrames(const uint64_t* pcs, size_t count, size_t skip,
                           uint64_t* frame_host_pcs, size_t frame_count,
                           uint64_t* out_stack_hash) {
    size_t copied = 0;
    if (skip < count) {
      copied = std::min(count - skip, frame_count);
      std::memcpy(frame_host_pcs, pcs + skip, copied * sizeof(uint64_t));
    }
    if (out_stack_hash) {
      *out_stack_hash = HashStack(frame_host_pcs, copied);
    }
    return copied;
  }

  backend::CodeCache* code_cache_;
  uintptr_t code_cache_min_;
  uintptr_t code_cache_max_;
};

std::unique_ptr<StackWalker> StackWalker::Create(
    backend::CodeCache* code_cache) {
  return std::make_unique<PosixStackWalker>(code_cache);
}

}  // namespace cpu
}  // namespace xe
