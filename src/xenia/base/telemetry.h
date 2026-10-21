/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_TELEMETRY_H_
#define XENIA_BASE_TELEMETRY_H_

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Output-only telemetry: JSON lines describing what the emulator is doing
// (state, frame pacing, compatibility events), written to a file when the
// telemetry_file cvar is set. Off by default; there is no input or control
// channel. The format is documented in docs/telemetry.md.
namespace xe::telemetry {

// Counters accumulated per frame_stats window and reset when it is written.
enum class Counter : uint32_t {
  kShaderCompiles,
  kPipelineCompiles,
  kFileReadBytes,
  kInputPolls,
  kAudioFrames,
  kAudioDropped,
  kCount,
};

// Gauges are current values (not reset per window).
enum class Gauge : uint32_t {
  kGuestThreads,
  kBlockedThreads,
  kSleepingThreads,
  kCount,
};

enum class State : uint8_t {
  kStarting,
  kLoading,
  kRunning,
  kSystemUi,
  kPaused,
  kCrashed,
  kExited,
};
const char* StateName(State state);

enum class Signal : uint8_t {
  kTitle,
  kSwap,
  kPause,
  kResume,
  kUiShown,
  kUiClosed,
  kCrash,
  kExit,
};

struct Transition {
  State from;
  State to;
  const char* reason;
};

// The emulator state machine (see docs/telemetry.md). Pure: no I/O and no
// locking, so it is unit-testable; the process-wide instance used by Notify is
// guarded by a mutex.
class StateMachine {
 public:
  State state() const { return state_; }
  // Returns the transition taken, or nullopt when the signal is ignored.
  std::optional<Transition> Apply(Signal signal);

 private:
  State state_ = State::kStarting;
  // The state left when entering paused or system_ui.
  State paused_from_ = State::kStarting;
  State ui_from_ = State::kStarting;
};

namespace detail {
extern std::atomic<bool> enabled_;
void CountSlow(Counter counter, uint64_t amount);
void AddGaugeSlow(Gauge gauge, int64_t delta);
void ObservePeakSlow(float abs_peak);
}  // namespace detail

// One relaxed load; safe to call on any hot path.
inline bool enabled() {
  return detail::enabled_.load(std::memory_order_relaxed);
}

inline void Count(Counter counter, uint64_t amount = 1) {
  if (enabled()) {
    detail::CountSlow(counter, amount);
  }
}

inline void AddGauge(Gauge gauge, int64_t delta) {
  if (enabled()) {
    detail::AddGaugeSlow(gauge, delta);
  }
}

// Atomic max of the window's absolute audio peak (0..1+), reset each window.
inline void ObservePeak(float abs_peak) {
  if (enabled()) {
    detail::ObservePeakSlow(abs_peak);
  }
}

// 20 * log10(peak) rounded to 1 decimal; nullopt when peak <= 0 (silence).
std::optional<double> PeakDb(float abs_peak);

// +1 on construction and -1 on destruction, only if telemetry was enabled at
// construction (so the gauge stays balanced if it is toggled in between).
class GaugeScope {
 public:
  explicit GaugeScope(Gauge gauge);
  ~GaugeScope();
  GaugeScope(const GaugeScope&) = delete;
  GaugeScope& operator=(const GaugeScope&) = delete;

 private:
  Gauge gauge_;
  bool active_;
};

// Frame hooks, called by the GPU. OnVblank drives frame_stats; OnSwap records
// swap intervals and moves loading -> running on the first swap.
void OnVblank(uint64_t vblank_count, uint64_t swap_count);
void OnSwap(uint64_t swap_count, uint64_t host_ns);
// Called by the presenter when a host present call returns with a guest
// output image not presented before; refresh_ns is when the guest output was
// refreshed with that image, at its swap.
void OnHostPresent(uint64_t refresh_ns, uint64_t present_ns);
// Who paused the emulator. Each source pauses and resumes on its own; the
// state stays paused while any source is paused.
enum class PauseSource : uint8_t {
  // Emulator::Pause, through GraphicsSystem (save/restore state, crash).
  kEmulator,
  // A debugger break (break, breakpoint, exception) until Processor::Continue.
  kDebugger,
};
// Pause or resume from one source. The state machine is paused when the first
// source pauses and resumed when the last one resumes. On pause, also drops the
// last swap time so the paused gap does not count as a frame interval.
void OnPause(PauseSource source, bool paused);

// Applies a signal to the process-wide state machine and writes a state event
// on a transition. No-op when disabled.
void Notify(Signal signal);
State CurrentState();

struct HostInfo {
  std::string os, os_version, arch, cpu;
  uint32_t cpu_cores = 0, cpu_threads = 0;
  uint64_t ram_mb = 0;
};
HostInfo QueryHostInfo();

// Host std::chrono::steady_clock time in ns (QueryPerformanceCounter on
// Windows, CLOCK_MONOTONIC on Linux).
uint64_t NowNs();

// Quotes and escapes a JSON string: '"', '\\' and control characters; invalid
// UTF-8 bytes become U+FFFD.
void AppendJsonString(std::string& out, std::string_view s);

// One event line under construction. Fields are written in call order after
// the envelope (type, vblank, swap, ns, state).
class Event {
 public:
  explicit Event(std::string_view type);

  Event& Str(std::string_view key, std::string_view value);
  Event& Int(std::string_view key, int64_t value);
  Event& UInt(std::string_view key, uint64_t value);
  // Writes the JSON string "0x%0*X".
  Event& Hex(std::string_view key, uint64_t value, int digits = 8);
  Event& Bool(std::string_view key, bool value);
  // value must already be valid JSON (an object, array, number or null).
  Event& Raw(std::string_view key, std::string_view json);

  // The full line, without the trailing '\n'.
  std::string Finish(uint64_t vblank, uint64_t swap, uint64_t ns,
                     State state) const;

 private:
  void Key(std::string_view key);

  std::string type_;
  std::string fields_;
};

// The line as Emit would write it now, with the current envelope, without
// writing it. For other channels that share the format.
std::string FinishNow(const Event& event);
// Queues an event for the writer thread. No-op when disabled.
void Emit(Event event);
// Writes everything queued, then this event, and flushes all sinks before
// returning (for crash and exit, when the process may be about to die). No-op
// when disabled.
void EmitNow(Event event);

// Reports a guest crash. The first call per session writes event like EmitNow,
// sets the exit reason to "crash" and moves the state to crashed; later calls
// write nothing (only the first crash is reported). Returns whether this call
// reported. No-op (false) when disabled.
bool ReportCrash(Event event);

// True the first time a (kind, key) pair is seen in this process.
bool FirstTime(std::string_view kind, std::string_view key);

class Sink {
 public:
  virtual ~Sink() = default;
  virtual void Write(std::string_view line) = 0;
  virtual void Flush() {}
};
void AddSink(std::unique_ptr<Sink> sink);

// Starts the writer and enables telemetry, writing to the sinks added so far
// and later. Writes nothing itself. Does nothing if already started.
void Start();
// Truncates and opens path (readable by other processes while it grows), adds
// it as a sink and starts telemetry if needed. Returns false and logs an error
// on failure, or if a file is already open.
bool OpenFile(const std::filesystem::path& path);

// The reason written by Shutdown: "shutdown" (default), "title_exit", "crash".
// "crash" is never replaced (closing a crashed title keeps it), and a title
// launch resets any other reason to "shutdown".
void SetExitReason(std::string_view reason);

// Registers a callback that adds fields to the exit event. Providers run inside
// Shutdown with telemetry locks held, so they must only fill the Event and must
// not call any other telemetry function. Thread-safe; providers are never
// removed. Register once per process (e.g. at kernel construction).
void AddExitField(std::function<void(Event&)> provider);

// Writes exit and the final state -> exited line synchronously, stops the
// writer, closes the sinks and disables telemetry. No-op when not enabled.
void Shutdown();

struct FrameStats {
  uint32_t window_vblanks;
  uint32_t swaps;
  std::vector<double> swap_ms;
  // Shader and pipeline compiles during each swap_ms interval.
  std::vector<uint64_t> swap_compiles;
  uint32_t presents = 0;
  // Intervals between host presents of new guest images, and from each
  // image's refresh to its present.
  std::vector<double> present_ms;
  std::vector<double> present_latency_ms;
};

// Swap-interval accounting for frame_stats. An interval is the gap between two
// consecutive swaps (ms, 2 decimals) and belongs to the window of the later
// swap. The first swap after construction or OnPause() has no interval. Host
// presents are counted the same way. Not thread-safe.
class FrameWindow {
 public:
  explicit FrameWindow(uint32_t window_vblanks);
  // compiles_total: shader and pipeline compiles so far, from any thread.
  void OnSwap(uint64_t host_ns, uint64_t compiles_total = 0);
  void OnPresent(uint64_t refresh_ns, uint64_t present_ns);
  void OnPause();
  // Returns the window's stats every window_vblanks-th call and resets it.
  std::optional<FrameStats> OnVblank();

 private:
  uint32_t window_vblanks_;
  uint32_t vblanks_ = 0;
  uint32_t swaps_ = 0;
  std::vector<double> swap_ms_;
  std::vector<uint64_t> swap_compiles_;
  std::optional<uint64_t> last_swap_ns_;
  uint64_t last_compiles_ = 0;
  uint32_t presents_ = 0;
  std::vector<double> present_ms_;
  std::vector<double> present_latency_ms_;
  std::optional<uint64_t> last_present_ns_;
};

}  // namespace xe::telemetry

#endif  // XENIA_BASE_TELEMETRY_H_
