/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/telemetry.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <utility>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"

#if XE_PLATFORM_WIN32
#include <share.h>
#include "xenia/base/platform_win.h"
#endif

#if XE_PLATFORM_LINUX
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <fstream>
#include <set>
#endif

#if XE_ARCH_AMD64
#if XE_COMPILER_MSVC
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

DEFINE_path(telemetry_file, "",
            "Write output-only telemetry events (JSONL) to this file. Empty "
            "disables telemetry.",
            "Telemetry");
DEFINE_int32(telemetry_window, 60, "Vblanks per frame_stats telemetry event.",
             "Telemetry");

namespace xe::telemetry {

namespace detail {
std::atomic<bool> enabled_{false};
}  // namespace detail

namespace {

constexpr size_t kCounterCount = size_t(Counter::kCount);
constexpr size_t kGaugeCount = size_t(Gauge::kCount);
// Wake the writer early when this many lines are queued; otherwise it drains
// on its 100 ms tick.
constexpr size_t kWakeWriterLines = 256;
constexpr auto kWriterInterval = std::chrono::milliseconds(100);

std::atomic<uint8_t> current_state_{uint8_t(State::kStarting)};
std::atomic<uint64_t> vblank_{0};
std::atomic<uint64_t> swap_{0};
std::array<std::atomic<uint64_t>, kCounterCount> counters_{};
// Shader and pipeline compiles since Start, never reset by a window.
std::atomic<uint64_t> compiles_total{0};
std::array<std::atomic<int64_t>, kGaugeCount> gauges_{};
std::atomic<float> peak_{0.0f};

// Non-trivial state lives in a leaked singleton so nothing is destroyed (and
// no joinable thread is torn down) during static destruction.
struct Globals {
  // Serializes OpenFile and Shutdown.
  std::mutex lifecycle_mutex;

  // Lock order: state_mutex -> write_mutex -> queue_mutex.
  std::mutex state_mutex;
  StateMachine machine;
  std::string exit_reason = "shutdown";
  // Bit per PauseSource currently paused.
  uint8_t pause_sources = 0;
  // Set by the first ReportCrash of the session.
  bool crash_reported = false;

  // Held while writing to the sinks, so lines leave in queue order and
  // EmitNow lines land after everything queued before them.
  std::mutex write_mutex;
  std::vector<std::unique_ptr<Sink>> sinks;
  // Set (under write_mutex) once Shutdown has written exit and exited, so no
  // later synchronous write can follow them.
  bool closed = true;
  // Under lifecycle_mutex.
  bool file_open = false;

  std::mutex queue_mutex;
  std::condition_variable queue_cv;
  std::vector<std::string> queue;
  bool accepting = false;
  bool stop = false;
  std::thread writer;

  std::mutex frame_mutex;
  std::optional<FrameWindow> frame_window;

  std::mutex first_mutex;
  std::unordered_set<std::string> first_seen;

  // Leaf lock: only held to copy or append to exit_fields.
  std::mutex exit_field_mutex;
  std::vector<std::function<void(Event&)>> exit_fields;
};

Globals& G() {
  static Globals* globals = new Globals();
  return *globals;
}

class FileSink : public Sink {
 public:
  explicit FileSink(FILE* file) : file_(file) {}
  ~FileSink() override { std::fclose(file_); }
  void Write(std::string_view line) override {
    std::fwrite(line.data(), 1, line.size(), file_);
    std::fputc('\n', file_);
  }
  void Flush() override { std::fflush(file_); }

 private:
  FILE* file_;
};

// Caller holds write_mutex.
void WriteLinesLocked(Globals& g, const std::vector<std::string>& lines) {
  for (const std::string& line : lines) {
    for (auto& sink : g.sinks) {
      sink->Write(line);
    }
  }
}

void FlushLocked(Globals& g) {
  for (auto& sink : g.sinks) {
    sink->Flush();
  }
}

std::vector<std::string> TakeQueue(Globals& g) {
  std::vector<std::string> lines;
  std::lock_guard<std::mutex> lock(g.queue_mutex);
  lines.swap(g.queue);
  return lines;
}

void Enqueue(std::string line) {
  Globals& g = G();
  bool wake;
  {
    std::lock_guard<std::mutex> lock(g.queue_mutex);
    if (!g.accepting) {
      return;
    }
    g.queue.push_back(std::move(line));
    wake = g.queue.size() >= kWakeWriterLines;
  }
  if (wake) {
    g.queue_cv.notify_one();
  }
}

// Writes everything queued, then line, and flushes, before returning.
void WriteNow(const std::string& line) {
  Globals& g = G();
  std::lock_guard<std::mutex> lock(g.write_mutex);
  if (g.closed) {
    return;
  }
  WriteLinesLocked(g, TakeQueue(g));
  for (auto& sink : g.sinks) {
    sink->Write(line);
  }
  FlushLocked(g);
}

// A state event line, stamped with its to state.
std::string StateLine(const Transition& transition, std::string_view reason) {
  return Event("state")
      .Str("from", StateName(transition.from))
      .Str("to", StateName(transition.to))
      .Str("reason", reason)
      .Finish(vblank_.load(std::memory_order_relaxed),
              swap_.load(std::memory_order_relaxed), NowNs(), transition.to);
}

void WriterThread() {
  Globals& g = G();
  while (true) {
    bool stop;
    {
      std::unique_lock<std::mutex> lock(g.queue_mutex);
      g.queue_cv.wait_for(lock, kWriterInterval, [&g] {
        return g.stop || g.queue.size() >= kWakeWriterLines;
      });
      stop = g.stop;
    }
    {
      std::lock_guard<std::mutex> lock(g.write_mutex);
      std::vector<std::string> lines = TakeQueue(g);
      if (!lines.empty()) {
        WriteLinesLocked(g, lines);
        FlushLocked(g);
      }
    }
    if (stop) {
      return;
    }
  }
}

std::string FormatDouble(double value) {
  if (!std::isfinite(value)) {
    return "null";
  }
  return fmt::format("{}", value);
}

void EmitFrameStats(const FrameStats& stats) {
  auto take = [](Counter counter) {
    return counters_[size_t(counter)].exchange(0, std::memory_order_relaxed);
  };
  auto gauge = [](Gauge g) {
    return gauges_[size_t(g)].load(std::memory_order_relaxed);
  };
  auto doubles = [](const std::vector<double>& values) {
    std::string json = "[";
    for (size_t i = 0; i < values.size(); ++i) {
      if (i) {
        json += ',';
      }
      json += FormatDouble(values[i]);
    }
    return json + ']';
  };
  std::string swap_compiles = "[";
  for (size_t i = 0; i < stats.swap_compiles.size(); ++i) {
    if (i) {
      swap_compiles += ',';
    }
    swap_compiles += std::to_string(stats.swap_compiles[i]);
  }
  swap_compiles += ']';
  std::optional<double> peak_db =
      PeakDb(peak_.exchange(0.0f, std::memory_order_relaxed));

  Event event("frame_stats");
  event.UInt("window_vblanks", stats.window_vblanks)
      .UInt("swaps", stats.swaps)
      .Raw("swap_ms", doubles(stats.swap_ms))
      .Raw("swap_compiles", swap_compiles)
      .UInt("presents", stats.presents)
      .Raw("present_ms", doubles(stats.present_ms))
      .Raw("present_latency_ms", doubles(stats.present_latency_ms))
      .UInt("shader_compiles", take(Counter::kShaderCompiles))
      .UInt("pipeline_compiles", take(Counter::kPipelineCompiles))
      .UInt("file_read_bytes", take(Counter::kFileReadBytes))
      .UInt("input_polls", take(Counter::kInputPolls))
      .Int("guest_threads", gauge(Gauge::kGuestThreads))
      .Int("blocked_threads", gauge(Gauge::kBlockedThreads))
      .Int("sleeping_threads", gauge(Gauge::kSleepingThreads))
      .UInt("audio_frames", take(Counter::kAudioFrames))
      .UInt("audio_dropped", take(Counter::kAudioDropped))
      .Raw("audio_peak_db", peak_db ? FormatDouble(*peak_db) : "null");
  Emit(std::move(event));
}

// Decodes one UTF-8 sequence at s[i]; returns its length, or 0 if invalid.
size_t ValidUtf8Length(std::string_view s, size_t i) {
  auto byte = [&](size_t k) { return uint8_t(s[k]); };
  uint8_t c = byte(i);
  size_t length;
  uint32_t min_code_point;
  uint32_t code_point;
  if (c < 0x80) {
    return 1;
  } else if ((c & 0xE0) == 0xC0) {
    length = 2;
    min_code_point = 0x80;
    code_point = c & 0x1F;
  } else if ((c & 0xF0) == 0xE0) {
    length = 3;
    min_code_point = 0x800;
    code_point = c & 0x0F;
  } else if ((c & 0xF8) == 0xF0) {
    length = 4;
    min_code_point = 0x10000;
    code_point = c & 0x07;
  } else {
    return 0;
  }
  if (i + length > s.size()) {
    return 0;
  }
  for (size_t k = 1; k < length; ++k) {
    uint8_t continuation = byte(i + k);
    if ((continuation & 0xC0) != 0x80) {
      return 0;
    }
    code_point = (code_point << 6) | (continuation & 0x3F);
  }
  if (code_point < min_code_point || code_point > 0x10FFFF ||
      (code_point >= 0xD800 && code_point <= 0xDFFF)) {
    return 0;
  }
  return length;
}

std::string Trim(std::string_view s) {
  const char* kSpace = " \t\r\n";
  size_t begin = s.find_first_not_of(kSpace);
  if (begin == std::string_view::npos) {
    return std::string();
  }
  size_t end = s.find_last_not_of(kSpace);
  return std::string(s.substr(begin, end - begin + 1));
}

#if XE_ARCH_AMD64
void Cpuid(uint32_t leaf, uint32_t regs[4]) {
#if XE_COMPILER_MSVC
  int out[4];
  __cpuid(out, int(leaf));
  for (int i = 0; i < 4; ++i) {
    regs[i] = uint32_t(out[i]);
  }
#else
  unsigned int a, b, c, d;
  __cpuid(leaf, a, b, c, d);
  regs[0] = a;
  regs[1] = b;
  regs[2] = c;
  regs[3] = d;
#endif
}

// The processor brand string, CPUID leaves 0x80000002-0x80000004.
std::string CpuidBrandString() {
  uint32_t regs[4];
  Cpuid(0x80000000, regs);
  if (regs[0] < 0x80000004) {
    return std::string();
  }
  char brand[49] = {};
  for (uint32_t i = 0; i < 3; ++i) {
    Cpuid(0x80000002 + i, regs);
    std::memcpy(brand + i * 16, regs, 16);
  }
  return Trim(brand);
}
#endif  // XE_ARCH_AMD64

}  // namespace

namespace detail {

void CountSlow(Counter counter, uint64_t amount) {
  counters_[size_t(counter)].fetch_add(amount, std::memory_order_relaxed);
  if (counter == Counter::kShaderCompiles ||
      counter == Counter::kPipelineCompiles) {
    compiles_total.fetch_add(amount, std::memory_order_relaxed);
  }
}

void AddGaugeSlow(Gauge gauge, int64_t delta) {
  gauges_[size_t(gauge)].fetch_add(delta, std::memory_order_relaxed);
}

void ObservePeakSlow(float abs_peak) {
  float current = peak_.load(std::memory_order_relaxed);
  while (abs_peak > current &&
         !peak_.compare_exchange_weak(current, abs_peak,
                                      std::memory_order_relaxed)) {
  }
}

}  // namespace detail

const char* StateName(State state) {
  switch (state) {
    case State::kStarting:
      return "starting";
    case State::kLoading:
      return "loading";
    case State::kRunning:
      return "running";
    case State::kSystemUi:
      return "system_ui";
    case State::kPaused:
      return "paused";
    case State::kCrashed:
      return "crashed";
    case State::kExited:
      return "exited";
  }
  return "unknown";
}

std::optional<Transition> StateMachine::Apply(Signal signal) {
  State from = state_;
  auto go = [&](State to, const char* reason) -> std::optional<Transition> {
    state_ = to;
    return Transition{from, to, reason};
  };
  if (from == State::kExited) {
    return std::nullopt;
  }
  if (from == State::kCrashed) {
    if (signal == Signal::kExit) {
      return go(State::kExited, "shutdown");
    }
    return std::nullopt;
  }
  switch (signal) {
    case Signal::kTitle:
      paused_from_ = State::kStarting;
      ui_from_ = State::kStarting;
      return go(State::kLoading, "title");
    case Signal::kSwap:
      if (from == State::kLoading) {
        return go(State::kRunning, "first_swap");
      }
      return std::nullopt;
    case Signal::kPause:
      if (from == State::kPaused) {
        return std::nullopt;
      }
      paused_from_ = from;
      return go(State::kPaused, "pause");
    case Signal::kResume:
      if (from != State::kPaused) {
        return std::nullopt;
      }
      return go(paused_from_, "resume");
    case Signal::kUiShown:
      if (from == State::kLoading || from == State::kRunning) {
        ui_from_ = from;
        return go(State::kSystemUi, "xam_ui");
      }
      if (from == State::kPaused && (paused_from_ == State::kLoading ||
                                     paused_from_ == State::kRunning)) {
        ui_from_ = paused_from_;
        paused_from_ = State::kSystemUi;
      }
      return std::nullopt;
    case Signal::kUiClosed:
      if (from == State::kSystemUi) {
        return go(ui_from_, "xam_ui_closed");
      }
      if (from == State::kPaused && paused_from_ == State::kSystemUi) {
        paused_from_ = ui_from_;
      }
      return std::nullopt;
    case Signal::kCrash:
      return go(State::kCrashed, "guest_crash");
    case Signal::kExit:
      return go(State::kExited, "shutdown");
  }
  return std::nullopt;
}

std::optional<double> PeakDb(float abs_peak) {
  if (!(abs_peak > 0.0f)) {
    return std::nullopt;
  }
  double db = 20.0 * std::log10(double(abs_peak));
  return std::round(db * 10.0) / 10.0;
}

GaugeScope::GaugeScope(Gauge gauge) : gauge_(gauge), active_(enabled()) {
  if (active_) {
    detail::AddGaugeSlow(gauge_, 1);
  }
}

GaugeScope::~GaugeScope() {
  if (active_) {
    detail::AddGaugeSlow(gauge_, -1);
  }
}

void OnVblank(uint64_t vblank_count, uint64_t swap_count) {
  if (!enabled()) {
    return;
  }
  vblank_.store(vblank_count, std::memory_order_relaxed);
  swap_.store(swap_count, std::memory_order_relaxed);
  std::optional<FrameStats> stats;
  {
    Globals& g = G();
    std::lock_guard<std::mutex> lock(g.frame_mutex);
    if (g.frame_window) {
      stats = g.frame_window->OnVblank();
    }
  }
  if (stats) {
    EmitFrameStats(*stats);
  }
}

void OnSwap(uint64_t swap_count, uint64_t host_ns) {
  if (!enabled()) {
    return;
  }
  swap_.store(swap_count, std::memory_order_relaxed);
  {
    Globals& g = G();
    std::lock_guard<std::mutex> lock(g.frame_mutex);
    if (g.frame_window) {
      g.frame_window->OnSwap(host_ns,
                             compiles_total.load(std::memory_order_relaxed));
    }
  }
  Notify(Signal::kSwap);
}

void OnHostPresent(uint64_t refresh_ns, uint64_t present_ns) {
  if (!enabled()) {
    return;
  }
  Globals& g = G();
  std::lock_guard<std::mutex> lock(g.frame_mutex);
  if (g.frame_window) {
    g.frame_window->OnPresent(refresh_ns, present_ns);
  }
}

namespace {

// Caller holds state_mutex and has checked enabled() under it.
void NotifyLocked(Globals& g, Signal signal) {
  if (signal == Signal::kTitle && g.exit_reason != "crash") {
    // A new title: an earlier title_exit no longer describes the session.
    g.exit_reason = "shutdown";
  }
  std::optional<Transition> transition = g.machine.Apply(signal);
  if (!transition) {
    return;
  }
  current_state_.store(uint8_t(transition->to), std::memory_order_relaxed);
  std::string_view reason = transition->reason;
  if (signal == Signal::kExit) {
    reason = g.exit_reason;
  }
  std::string line = StateLine(*transition, reason);
  // Still under state_mutex, so state lines are written in transition order.
  if (transition->to == State::kCrashed || transition->to == State::kExited) {
    WriteNow(line);
  } else {
    Enqueue(std::move(line));
  }
  // A title launched while a pause source is still paused (e.g. a debugger
  // break at start) stays paused.
  if (signal == Signal::kTitle && g.pause_sources) {
    NotifyLocked(g, Signal::kPause);
  }
}

}  // namespace

void OnPause(PauseSource source, bool paused) {
  if (!enabled()) {
    return;
  }
  Globals& g = G();
  if (paused) {
    std::lock_guard<std::mutex> lock(g.frame_mutex);
    if (g.frame_window) {
      g.frame_window->OnPause();
    }
  }
  std::lock_guard<std::mutex> lock(g.state_mutex);
  if (!enabled()) {
    return;
  }
  const uint8_t bit = uint8_t(1u << uint8_t(source));
  const uint8_t before = g.pause_sources;
  g.pause_sources = paused ? uint8_t(before | bit) : uint8_t(before & ~bit);
  if (!before && g.pause_sources) {
    NotifyLocked(g, Signal::kPause);
  } else if (before && !g.pause_sources) {
    NotifyLocked(g, Signal::kResume);
  }
}

State CurrentState() {
  return State(current_state_.load(std::memory_order_relaxed));
}

void Notify(Signal signal) {
  if (!enabled()) {
    return;
  }
  // Only the first swap after loading changes state; skip the mutex otherwise.
  if (signal == Signal::kSwap && CurrentState() != State::kLoading) {
    return;
  }
  Globals& g = G();
  std::lock_guard<std::mutex> lock(g.state_mutex);
  // Shutdown clears enabled under state_mutex; re-check so a caller that
  // raced it cannot change the machine after it was closed or reset.
  if (!enabled()) {
    return;
  }
  NotifyLocked(g, signal);
}

HostInfo QueryHostInfo() {
  HostInfo info;
#if XE_ARCH_AMD64
  info.arch = "x86_64";
#elif XE_ARCH_ARM64
  info.arch = "aarch64";
#else
  info.arch = "unknown";
#endif
  info.cpu_threads = std::thread::hardware_concurrency();
  if (!info.cpu_threads) {
    info.cpu_threads = 1;
  }

#if XE_PLATFORM_WIN32
  info.os = "windows";
  {
    // RtlGetVersion reports the real version regardless of the manifest.
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto rtl_get_version = ntdll ? reinterpret_cast<RtlGetVersionFn>(
                                       GetProcAddress(ntdll, "RtlGetVersion"))
                                 : nullptr;
    RTL_OSVERSIONINFOW version = {};
    version.dwOSVersionInfoSize = sizeof(version);
    if (rtl_get_version && rtl_get_version(&version) == 0) {
      info.os_version =
          fmt::format("{}.{}.{}", version.dwMajorVersion,
                      version.dwMinorVersion, version.dwBuildNumber);
    }
  }
  {
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    std::vector<uint8_t> buffer(length);
    auto* records = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
        buffer.data());
    if (length && GetLogicalProcessorInformationEx(RelationProcessorCore,
                                                   records, &length)) {
      uint32_t cores = 0;
      for (DWORD offset = 0; offset < length;) {
        auto* record =
            reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
                buffer.data() + offset);
        if (record->Relationship == RelationProcessorCore) {
          ++cores;
        }
        offset += record->Size;
      }
      info.cpu_cores = cores;
    }
  }
  {
    MEMORYSTATUSEX memory = {};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
      info.ram_mb = memory.ullTotalPhys / (1024 * 1024);
    }
  }
#if XE_ARCH_AMD64
  info.cpu = CpuidBrandString();
#else
  {
    wchar_t name[256] = {};
    DWORD size = sizeof(name);
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
                     L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, name,
                     &size) == ERROR_SUCCESS) {
      std::string narrow;
      for (const wchar_t* c = name; *c; ++c) {
        narrow += *c < 0x80 ? char(*c) : '?';
      }
      info.cpu = Trim(narrow);
    }
  }
#endif

#elif XE_PLATFORM_LINUX
  info.os = "linux";
  {
    struct utsname name;
    if (uname(&name) == 0) {
      info.os_version = name.release;
    }
  }
  {
    struct sysinfo system_info;
    if (sysinfo(&system_info) == 0) {
      info.ram_mb =
          uint64_t(system_info.totalram) * system_info.mem_unit / (1024 * 1024);
    }
  }
  {
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    std::string cpu_name;
    std::set<std::pair<std::string, std::string>> cores;
    std::string physical_id;
    auto value_of = [](std::string_view text) {
      size_t colon = text.find(':');
      return colon == std::string_view::npos ? std::string()
                                             : Trim(text.substr(colon + 1));
    };
    while (std::getline(cpuinfo, line)) {
      if (cpu_name.empty() && (line.rfind("model name", 0) == 0 ||
                               line.rfind("Hardware", 0) == 0)) {
        cpu_name = value_of(line);
      } else if (line.rfind("physical id", 0) == 0) {
        physical_id = value_of(line);
      } else if (line.rfind("core id", 0) == 0) {
        cores.emplace(physical_id, value_of(line));
      }
    }
    info.cpu_cores = uint32_t(cores.size());
#if XE_ARCH_AMD64
    info.cpu = CpuidBrandString();
#endif
    if (info.cpu.empty()) {
      info.cpu = cpu_name;
    }
  }
#endif

  if (!info.cpu_cores) {
    info.cpu_cores = info.cpu_threads;
  }
  // hardware_concurrency may only see one processor group.
  info.cpu_cores = std::min(info.cpu_cores, info.cpu_threads);
  if (info.os.empty()) {
    info.os = "unknown";
  }
  if (info.os_version.empty()) {
    info.os_version = "unknown";
  }
  if (info.cpu.empty()) {
    info.cpu = "unknown";
  }
  return info;
}

uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

void AppendJsonString(std::string& out, std::string_view s) {
  out += '"';
  for (size_t i = 0; i < s.size();) {
    uint8_t c = uint8_t(s[i]);
    if (c >= 0x80) {
      size_t length = ValidUtf8Length(s, i);
      if (length) {
        out.append(s.substr(i, length));
        i += length;
      } else {
        out += "\xEF\xBF\xBD";  // U+FFFD
        ++i;
      }
      continue;
    }
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          out += fmt::format("\\u{:04x}", unsigned(c));
        } else {
          out += char(c);
        }
        break;
    }
    ++i;
  }
  out += '"';
}

Event::Event(std::string_view type) : type_(type) {}

void Event::Key(std::string_view key) {
  fields_ += ',';
  AppendJsonString(fields_, key);
  fields_ += ':';
}

Event& Event::Str(std::string_view key, std::string_view value) {
  Key(key);
  AppendJsonString(fields_, value);
  return *this;
}

Event& Event::Int(std::string_view key, int64_t value) {
  Key(key);
  fields_ += fmt::format("{}", value);
  return *this;
}

Event& Event::UInt(std::string_view key, uint64_t value) {
  Key(key);
  fields_ += fmt::format("{}", value);
  return *this;
}

Event& Event::Hex(std::string_view key, uint64_t value, int digits) {
  Key(key);
  fields_ += fmt::format("\"0x{:0{}X}\"", value, digits);
  return *this;
}

Event& Event::Bool(std::string_view key, bool value) {
  Key(key);
  fields_ += value ? "true" : "false";
  return *this;
}

Event& Event::Raw(std::string_view key, std::string_view json) {
  Key(key);
  fields_ += json;
  return *this;
}

std::string Event::Finish(uint64_t vblank, uint64_t swap, uint64_t ns,
                          State state) const {
  std::string line;
  line.reserve(96 + type_.size() + fields_.size());
  line += "{\"type\":";
  AppendJsonString(line, type_);
  line += fmt::format(",\"vblank\":{},\"swap\":{},\"ns\":{},\"state\":\"{}\"",
                      vblank, swap, ns, StateName(state));
  line += fields_;
  line += '}';
  return line;
}

std::string FinishNow(const Event& event) {
  return event.Finish(vblank_.load(std::memory_order_relaxed),
                      swap_.load(std::memory_order_relaxed), NowNs(),
                      CurrentState());
}

void Emit(Event event) {
  if (!enabled()) {
    return;
  }
  Enqueue(FinishNow(event));
}

void EmitNow(Event event) {
  if (!enabled()) {
    return;
  }
  WriteNow(event.Finish(vblank_.load(std::memory_order_relaxed),
                        swap_.load(std::memory_order_relaxed), NowNs(),
                        CurrentState()));
}

bool ReportCrash(Event event) {
  if (!enabled()) {
    return false;
  }
  Globals& g = G();
  {
    std::lock_guard<std::mutex> lock(g.state_mutex);
    if (!enabled() || g.crash_reported) {
      return false;
    }
    g.crash_reported = true;
  }
  EmitNow(std::move(event));
  SetExitReason("crash");
  Notify(Signal::kCrash);
  return true;
}

bool FirstTime(std::string_view kind, std::string_view key) {
  std::string combined;
  combined.reserve(kind.size() + 1 + key.size());
  combined.append(kind);
  combined += '\x1f';
  combined.append(key);
  Globals& g = G();
  std::lock_guard<std::mutex> lock(g.first_mutex);
  return g.first_seen.insert(std::move(combined)).second;
}

void AddSink(std::unique_ptr<Sink> sink) {
  if (!sink) {
    return;
  }
  Globals& g = G();
  std::lock_guard<std::mutex> lock(g.write_mutex);
  g.sinks.push_back(std::move(sink));
}

namespace {

// Caller holds lifecycle_mutex, with telemetry disabled.
void StartLocked(Globals& g) {
  {
    std::lock_guard<std::mutex> lock(g.write_mutex);
    g.closed = false;
  }
  {
    std::lock_guard<std::mutex> lock(g.frame_mutex);
    g.frame_window.emplace(
        uint32_t(std::max(int32_t(1), cvars::telemetry_window)));
  }
  for (auto& counter : counters_) {
    counter.store(0, std::memory_order_relaxed);
  }
  compiles_total.store(0, std::memory_order_relaxed);
  peak_.store(0.0f, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(g.queue_mutex);
    g.queue.clear();
    g.accepting = true;
    g.stop = false;
  }
  g.writer = std::thread(WriterThread);
  detail::enabled_.store(true, std::memory_order_relaxed);

  // The app ends with std::quick_exit (EmulatorApp::OnDestroy) without
  // destroying the Emulator, so also write the exit lines from the exit
  // handlers. Shutdown is a no-op once telemetry is closed.
  static bool exit_handlers_registered = false;
  if (!exit_handlers_registered) {
    exit_handlers_registered = true;
    std::at_quick_exit(Shutdown);
    std::atexit(Shutdown);
  }
}

}  // namespace

void Start() {
  Globals& g = G();
  std::lock_guard<std::mutex> lifecycle_lock(g.lifecycle_mutex);
  if (!enabled()) {
    StartLocked(g);
  }
}

bool OpenFile(const std::filesystem::path& path) {
  Globals& g = G();
  std::lock_guard<std::mutex> lifecycle_lock(g.lifecycle_mutex);
  if (g.file_open) {
    XELOGE("Telemetry: already writing a file, not opening {}",
           xe::path_to_utf8(path));
    return false;
  }
  // Truncate, and let other processes read the file while it grows.
#if XE_PLATFORM_WIN32
  FILE* file = _wfsopen(path.c_str(), L"wb", _SH_DENYWR);
#else
  FILE* file = std::fopen(path.c_str(), "wb");
#endif
  if (!file) {
    XELOGE("Telemetry: failed to open {} for writing", xe::path_to_utf8(path));
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(g.write_mutex);
    g.sinks.push_back(std::make_unique<FileSink>(file));
  }
  g.file_open = true;
  if (!enabled()) {
    StartLocked(g);
  }
  return true;
}

void AddExitField(std::function<void(Event&)> provider) {
  if (!provider) {
    return;
  }
  Globals& g = G();
  std::lock_guard<std::mutex> lock(g.exit_field_mutex);
  g.exit_fields.push_back(std::move(provider));
}

void SetExitReason(std::string_view reason) {
  Globals& g = G();
  std::lock_guard<std::mutex> lock(g.state_mutex);
  // A crash outranks any later reason, e.g. closing the crashed title.
  if (g.exit_reason == "crash") {
    return;
  }
  g.exit_reason = std::string(reason);
}

void Shutdown() {
  Globals& g = G();
  std::lock_guard<std::mutex> lifecycle_lock(g.lifecycle_mutex);
  if (!enabled()) {
    return;
  }
  // exit and state -> exited must be the last two lines, even while GPU and
  // guest threads keep emitting (the quick_exit path). Under state_mutex and
  // write_mutex: disable, stop accepting queued lines, write what was queued
  // before this point, then exit and exited, and mark the sinks closed so a
  // racing EmitNow is dropped.
  {
    std::lock_guard<std::mutex> state_lock(g.state_mutex);
    uint64_t vblank_total = vblank_.load(std::memory_order_relaxed);
    uint64_t swap_total = swap_.load(std::memory_order_relaxed);
    Event exit_event("exit");
    exit_event.Str("reason", g.exit_reason)
        .UInt("vblank_total", vblank_total)
        .UInt("swap_total", swap_total);
    // Providers only fill the event; they must not call back into telemetry
    // (the lifecycle and state locks are held).
    std::vector<std::function<void(Event&)>> providers;
    {
      std::lock_guard<std::mutex> lock(g.exit_field_mutex);
      providers = g.exit_fields;
    }
    for (auto& provider : providers) {
      provider(exit_event);
    }
    std::string exit_line =
        exit_event.Finish(vblank_total, swap_total, NowNs(), CurrentState());
    std::string state_line;
    if (std::optional<Transition> transition = g.machine.Apply(Signal::kExit)) {
      current_state_.store(uint8_t(transition->to), std::memory_order_relaxed);
      state_line = StateLine(*transition, g.exit_reason);
    }
    detail::enabled_.store(false, std::memory_order_relaxed);

    std::lock_guard<std::mutex> write_lock(g.write_mutex);
    std::vector<std::string> pending;
    {
      std::lock_guard<std::mutex> lock(g.queue_mutex);
      g.accepting = false;
      g.stop = true;
      pending.swap(g.queue);
    }
    WriteLinesLocked(g, pending);
    for (auto& sink : g.sinks) {
      sink->Write(exit_line);
      if (!state_line.empty()) {
        sink->Write(state_line);
      }
    }
    FlushLocked(g);
    g.closed = true;
  }
  g.queue_cv.notify_one();
  if (g.writer.joinable()) {
    g.writer.join();
  }
  {
    std::lock_guard<std::mutex> lock(g.write_mutex);
    g.sinks.clear();
  }
  g.file_open = false;
  {
    std::lock_guard<std::mutex> lock(g.frame_mutex);
    g.frame_window.reset();
  }
  // Start a later OpenFile in this process from a clean state machine.
  {
    std::lock_guard<std::mutex> lock(g.state_mutex);
    g.machine = StateMachine();
    g.exit_reason = "shutdown";
    g.pause_sources = 0;
    g.crash_reported = false;
    current_state_.store(uint8_t(State::kStarting), std::memory_order_relaxed);
  }
}

FrameWindow::FrameWindow(uint32_t window_vblanks)
    : window_vblanks_(std::max(window_vblanks, uint32_t(1))) {}

namespace {
// ns -> ms with 2 decimals.
double IntervalMs(uint64_t from_ns, uint64_t to_ns) {
  return std::round(double(to_ns - from_ns) / 10000.0) / 100.0;
}
}  // namespace

void FrameWindow::OnSwap(uint64_t host_ns, uint64_t compiles_total) {
  ++swaps_;
  if (last_swap_ns_ && host_ns >= *last_swap_ns_) {
    swap_ms_.push_back(IntervalMs(*last_swap_ns_, host_ns));
    swap_compiles_.push_back(
        compiles_total >= last_compiles_ ? compiles_total - last_compiles_ : 0);
  }
  last_swap_ns_ = host_ns;
  last_compiles_ = compiles_total;
}

void FrameWindow::OnPresent(uint64_t refresh_ns, uint64_t present_ns) {
  ++presents_;
  if (last_present_ns_ && present_ns >= *last_present_ns_) {
    present_ms_.push_back(IntervalMs(*last_present_ns_, present_ns));
  }
  if (present_ns >= refresh_ns) {
    present_latency_ms_.push_back(IntervalMs(refresh_ns, present_ns));
  }
  last_present_ns_ = present_ns;
}

void FrameWindow::OnPause() {
  last_swap_ns_.reset();
  last_present_ns_.reset();
}

std::optional<FrameStats> FrameWindow::OnVblank() {
  if (++vblanks_ < window_vblanks_) {
    return std::nullopt;
  }
  FrameStats stats{window_vblanks_,
                   swaps_,
                   std::move(swap_ms_),
                   std::move(swap_compiles_),
                   presents_,
                   std::move(present_ms_),
                   std::move(present_latency_ms_)};
  vblanks_ = 0;
  swaps_ = 0;
  presents_ = 0;
  swap_ms_.clear();
  swap_compiles_.clear();
  present_ms_.clear();
  present_latency_ms_.clear();
  return stats;
}

}  // namespace xe::telemetry
