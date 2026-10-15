/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/threading.h"
#include "xenia/cpu/testing/util.h"
#include "xenia/cpu/thread.h"

using namespace xe;
using namespace xe::cpu;
using namespace xe::cpu::hir;
using namespace xe::cpu::testing;

namespace {
// A guest thread record backed by a parked host thread, as XThread would
// register it with the processor.
class ParkedThread : public cpu::Thread {
 public:
  ParkedThread(ThreadState* thread_state, threading::Event* release) {
    thread_state_ = thread_state;
    thread_ = threading::Thread::Create(
        {}, [release] { threading::Wait(release, false); });
  }
};
}  // namespace

// StackWalker::Create returns null on POSIX, but StepToGuestSafePoint
// dereferenced the walker anyway, so KernelState::TerminateTitle and
// XThread::Save crashed on Linux for any running guest thread. Without a
// walker it must report that no safe point was reached. Platforms with a
// walker step the thread for real, which needs JIT code, so they're skipped.
TEST_CASE("StepToGuestSafePoint without a stack walker", "[processor]") {
  TestFunction test([](HIRBuilder& b) { b.Return(); });
  for (auto& processor : test.processors) {
    if (processor->stack_walker()) {
      continue;
    }
    const uint32_t kThreadId = 0x100;
    ThreadState thread_state(processor.get(), kThreadId, 0);
    auto release = threading::Event::CreateManualResetEvent(false);
    ParkedThread thread(&thread_state, release.get());
    processor->OnThreadCreated(0, &thread_state, &thread);

    REQUIRE(processor->StepToGuestSafePoint(kThreadId) == 0);

    processor->OnThreadDestroyed(kThreadId);
    release->Set();
    threading::Wait(thread.thread(), false);
  }
}
