/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <atomic>

#include "xenia/base/byte_order.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/raw_module.h"
#include "xenia/cpu/stack_walker.h"
#include "xenia/cpu/testing/util.h"
#include "xenia/cpu/thread.h"

using namespace xe;
using namespace xe::cpu;
using namespace xe::cpu::hir;
using namespace xe::cpu::testing;
using namespace std::chrono_literals;

namespace {

constexpr uint32_t kGuestEntry = 0x80000000;

std::atomic<bool> builtin_entered;
std::atomic<bool> builtin_released;

void BlockingBuiltin(ppc::PPCContext* ctx, void* arg0, void* arg1) {
  builtin_entered = true;
  while (!builtin_released) {
    threading::MaybeYield();
  }
}

// Runs a guest function on its own host thread.
class GuestThread {
 public:
  // The generator gets BlockingBuiltin to call.
  explicit GuestThread(
      std::function<void(HIRBuilder& b, Function* blocking_builtin)> generator)
      : memory_(std::make_unique<Memory>()) {
    memory_->Initialize();
    std::unique_ptr<backend::Backend> backend;
#if XE_ARCH_AMD64
    backend.reset(new backend::x64::X64Backend());
#elif XE_ARCH_ARM64
    backend.reset(new backend::a64::A64Backend());
#endif
    processor_ = std::make_unique<Processor>(memory_.get(), nullptr);
    processor_->Setup(std::move(backend));
    Function* blocking_builtin = processor_->DefineBuiltin(
        "BlockingBuiltin", BlockingBuiltin, nullptr, nullptr);
    processor_->AddModule(std::make_unique<TestModule>(
        processor_.get(), "Test",
        [](uint32_t address) { return address == kGuestEntry; },
        [generator, blocking_builtin](HIRBuilder& b) {
          generator(b, blocking_builtin);
          return true;
        },
        /*skip_cf_simplification=*/true));
    processor_->backend()->CommitExecutableRange(kGuestEntry,
                                                 kGuestEntry + 0x10000);
    function_ = processor_->ResolveFunction(kGuestEntry);
  }

  ~GuestThread() {
    if (thread_) {
      threading::Wait(thread_.get(), false, 10s);
    }
    if (stack_address_) {
      memory_->SystemHeapFree(stack_address_);
    }
  }

  void Start() {
    stack_address_ = memory_->SystemHeapAlloc(kStackSize);
    thread_ = threading::Thread::Create({}, [this] {
      ThreadState thread_state(processor_.get(), 0x100,
                               stack_address_ + kStackSize);
      context_ = thread_state.context();
      context_.load()->lr = 0xBCBCBCBC;
      context_.load()->r[3] = 0;
      started_ = true;
      function_->Call(&thread_state, uint32_t(context_.load()->lr));
    });
    while (!started_) {
      threading::MaybeYield();
    }
  }

  // Walks the suspended thread's stack.
  size_t Walk(StackFrame* frames, size_t frame_count,
              HostThreadContext* out_context = nullptr) {
    uint64_t pcs[64];
    auto walker = processor_->stack_walker();
    size_t count = walker->CaptureStackTrace(
        thread_.get(), pcs, 0, std::min(frame_count, xe::countof(pcs)), nullptr,
        out_context);
    walker->ResolveStack(pcs, frames, count);
    return count;
  }

  Processor* processor() const { return processor_.get(); }
  Function* function() const { return function_; }
  threading::Thread* thread() const { return thread_.get(); }
  // Ends the guest spin loop.
  void Stop() { context_.load()->r[3] = 1; }

 private:
  static constexpr uint32_t kStackSize = 64 * 1024;

  std::unique_ptr<Memory> memory_;
  std::unique_ptr<Processor> processor_;
  Function* function_ = nullptr;
  uint32_t stack_address_ = 0;
  std::unique_ptr<threading::Thread> thread_;
  std::atomic<ppc::PPCContext*> context_{nullptr};
  std::atomic<bool> started_{false};
};

ptrdiff_t FindGuestFrame(const StackFrame* frames, size_t count,
                         const Function* function) {
  for (size_t i = 0; i < count; ++i) {
    if (frames[i].type == StackFrame::Type::kGuest &&
        frames[i].guest_symbol.function == function) {
      return ptrdiff_t(i);
    }
  }
  return -1;
}

}  // namespace

TEST_CASE("STACK_WALKER_GUEST_FRAME", "[stack_walker]") {
  // Spin in guest code until r3 is set.
  GuestThread guest([](HIRBuilder& b, Function* blocking_builtin) {
    auto loop = b.NewLabel();
    b.MarkLabel(loop);
    b.BranchFalse(LoadGPR(b, 3), loop);
    b.Return();
  });
  REQUIRE(guest.function());
  REQUIRE(guest.processor()->stack_walker());
  guest.Start();

  // The thread may still be entering guest code, so retry until it's caught
  // in the loop.
  StackFrame frames[64];
  size_t count = 0;
  HostThreadContext context = {};
  bool in_loop = false;
  for (int attempt = 0; attempt < 100 && !in_loop; ++attempt) {
    threading::Sleep(1ms);
    guest.thread()->Suspend();
    count = guest.Walk(frames, xe::countof(frames), &context);
    in_loop = count && frames[0].type == StackFrame::Type::kGuest &&
              frames[0].guest_symbol.function == guest.function();
    guest.thread()->Resume();
  }
  guest.Stop();

  REQUIRE(in_loop);
#if XE_ARCH_AMD64
  REQUIRE(context.rip == frames[0].host_pc);
#elif XE_ARCH_ARM64
  REQUIRE(context.pc == frames[0].host_pc);
#endif
  // The walk continues through the host-to-guest thunk into host code.
  REQUIRE(count > 2);
  REQUIRE(frames[count - 1].type == StackFrame::Type::kHost);
}

TEST_CASE("STACK_WALKER_HOST_FRAME_ABOVE_GUEST", "[stack_walker]") {
  // Call a builtin that blocks, so the thread is stopped in host code called
  // from the guest, like a guest thread inside a kernel export.
  builtin_entered = false;
  builtin_released = false;
  GuestThread guest([](HIRBuilder& b, Function* blocking_builtin) {
    b.CallExtern(blocking_builtin);
    b.Return();
  });
  REQUIRE(guest.function());
  guest.Start();
  while (!builtin_entered) {
    threading::MaybeYield();
  }

  guest.thread()->Suspend();
  StackFrame frames[64];
  size_t count = guest.Walk(frames, xe::countof(frames));
  guest.thread()->Resume();
  builtin_released = true;

  REQUIRE(count > 0);
  REQUIRE(frames[0].type == StackFrame::Type::kHost);
  REQUIRE(FindGuestFrame(frames, count, guest.function()) > 0);
}

namespace {

// A cpu::Thread the processor can step, as for a kernel XThread.
class SteppableThread : public cpu::Thread {
 public:
  void set_thread(std::unique_ptr<threading::Thread> thread) {
    thread_ = std::move(thread);
  }
  void set_thread_state(ThreadState* thread_state) {
    thread_state_ = thread_state;
  }
  // Makes this the calling thread's cpu::Thread, as XThread does.
  void Bind() { current_thread_ = this; }
};

}  // namespace

TEST_CASE("STACK_WALKER_STEP_TO_GUEST_SAFE_POINT", "[stack_walker]") {
  auto memory = std::make_unique<Memory>();
  memory->Initialize();
  std::unique_ptr<backend::Backend> backend;
#if XE_ARCH_AMD64
  backend.reset(new backend::x64::X64Backend());
#elif XE_ARCH_ARM64
  backend.reset(new backend::a64::A64Backend());
#endif
  auto processor = std::make_unique<Processor>(memory.get(), nullptr);
  processor->Setup(std::move(backend));

  // Spin until the word r3 points to is nonzero:
  //   loop: lwz r4, 0(r3)
  //         cmpwi r4, 0
  //         beq loop
  //         blr
  constexpr uint32_t kCode = 0x82000000;
  constexpr uint32_t kCodeSize = 0x1000;
  REQUIRE(memory->LookupHeap(kCode)->AllocFixed(
      kCode, kCodeSize, 0, kMemoryAllocationReserve | kMemoryAllocationCommit,
      kMemoryProtectRead | kMemoryProtectWrite));
  const uint32_t code[] = {0x80830000, 0x2C040000, 0x4182FFF8, 0x4E800020};
  for (size_t i = 0; i < xe::countof(code); ++i) {
    xe::store_and_swap<uint32_t>(
        memory->TranslateVirtual(kCode + uint32_t(i * 4)), code[i]);
  }
  auto module = std::make_unique<RawModule>(processor.get());
  module->SetAddressRange(kCode, kCodeSize);
  processor->AddModule(std::move(module));
  auto function = processor->ResolveFunction(kCode);
  REQUIRE(function);

  uint32_t flag_address = memory->SystemHeapAlloc(4);
  xe::store_and_swap<uint32_t>(memory->TranslateVirtual(flag_address), 0);
  constexpr uint32_t kStackSize = 64 * 1024;
  uint32_t stack_address = memory->SystemHeapAlloc(kStackSize);

  constexpr uint32_t kThreadId = 0x100;
  SteppableThread guest_thread;
  std::atomic<bool> started(false);
  threading::Thread::CreationParameters params;
  params.create_suspended = true;
  guest_thread.set_thread(threading::Thread::Create(params, [&] {
    ThreadState thread_state(processor.get(), kThreadId,
                             stack_address + kStackSize);
    guest_thread.set_thread_state(&thread_state);
    guest_thread.Bind();
    auto ctx = thread_state.context();
    ctx->lr = 0xBCBCBCBC;
    ctx->r[3] = flag_address;
    processor->OnThreadCreated(1, &thread_state, &guest_thread);
    started = true;
    function->Call(&thread_state, uint32_t(ctx->lr));
    processor->OnThreadExit(kThreadId);
  }));
  guest_thread.thread()->Resume();
  while (!started) {
    threading::MaybeYield();
  }
  threading::Sleep(10ms);

  // As KernelState::TerminateTitle does.
  guest_thread.thread()->Suspend();
  uint32_t pc = processor->StepToGuestSafePoint(kThreadId);

  // Let the loop finish.
  xe::store_and_swap<uint32_t>(memory->TranslateVirtual(flag_address), 1);
  uint32_t suspend_count = 1;
  while (suspend_count) {
    guest_thread.thread()->Resume(&suspend_count);
  }
  auto wait_result = threading::Wait(guest_thread.thread(), false, 5s);

  // Stopped on the beq and stepped into its target, the loop start.
  REQUIRE(pc == kCode);
  REQUIRE(wait_result == threading::WaitResult::kSuccess);
  processor->OnThreadDestroyed(kThreadId);
}
