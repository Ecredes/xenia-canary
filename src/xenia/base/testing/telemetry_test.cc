/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/telemetry.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "xenia/base/platform.h"

#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#endif

#include "third_party/catch/include/catch.hpp"

namespace xe::base::test {

// The module is a process-wide singleton, so these cases depend on
// declaration order: "disabled by default" must run before any OpenFile.

TEST_CASE("telemetry: AppendJsonString escapes", "[telemetry]") {
  std::string out;
  xe::telemetry::AppendJsonString(out, "game:\\a\"b\n\x01");
  REQUIRE(out == "\"game:\\\\a\\\"b\\n\\u0001\"");
  out.clear();
  xe::telemetry::AppendJsonString(out, std::string_view("\xff", 1));
  REQUIRE(out == "\"\xef\xbf\xbd\"");
}

TEST_CASE("telemetry: Event envelope order", "[telemetry]") {
  auto line = xe::telemetry::Event("title")
                  .Str("title_id", "4D5307FF")
                  .Hex("media_id", 0x1234ABCD)
                  .Finish(10, 3, 999, xe::telemetry::State::kLoading);
  REQUIRE(line ==
          R"({"type":"title","vblank":10,"swap":3,"ns":999,"state":"loading",)"
          R"("title_id":"4D5307FF","media_id":"0x1234ABCD"})");
}

TEST_CASE("telemetry: FrameWindow intervals and windows", "[telemetry]") {
  xe::telemetry::FrameWindow w(3);
  w.OnSwap(1'000'000);  // first swap: no interval
  REQUIRE_FALSE(w.OnVblank());
  w.OnSwap(17'670'000);  // 16.67 ms
  REQUIRE_FALSE(w.OnVblank());
  auto s = w.OnVblank();
  REQUIRE(s);
  REQUIRE(s->swaps == 2);
  REQUIRE(s->swap_ms == std::vector<double>{16.67});
  REQUIRE(s->window_vblanks == 3);
}

TEST_CASE("telemetry: FrameWindow empty window and pause", "[telemetry]") {
  xe::telemetry::FrameWindow w(1);
  auto s = w.OnVblank();
  REQUIRE(s->swaps == 0);
  REQUIRE(s->swap_ms.empty());
  w.OnSwap(0);
  w.OnPause();
  w.OnSwap(5'000'000'000);
  s = w.OnVblank();
  REQUIRE(s->swaps == 2);
  REQUIRE(s->swap_ms.empty());
}

TEST_CASE("telemetry: StateMachine boot path", "[telemetry]") {
  using namespace xe::telemetry;
  StateMachine m;
  REQUIRE(m.state() == State::kStarting);
  REQUIRE_FALSE(m.Apply(Signal::kSwap));
  auto t = m.Apply(Signal::kTitle);
  REQUIRE(t->from == State::kStarting);
  REQUIRE(t->to == State::kLoading);
  REQUIRE(std::string_view(t->reason) == "title");
  REQUIRE(m.Apply(Signal::kSwap)->to == State::kRunning);
  REQUIRE_FALSE(m.Apply(Signal::kSwap));
}

TEST_CASE("telemetry: StateMachine pause and UI overlap", "[telemetry]") {
  using namespace xe::telemetry;
  StateMachine m;
  m.Apply(Signal::kTitle);
  m.Apply(Signal::kSwap);
  REQUIRE(m.Apply(Signal::kUiShown)->to == State::kSystemUi);
  REQUIRE(m.Apply(Signal::kPause)->to == State::kPaused);
  REQUIRE_FALSE(m.Apply(Signal::kUiClosed));  // only the remembered state
  REQUIRE(m.Apply(Signal::kResume)->to == State::kRunning);
  REQUIRE(m.Apply(Signal::kUiClosed) == std::nullopt);
}

TEST_CASE("telemetry: StateMachine terminal states", "[telemetry]") {
  using namespace xe::telemetry;
  StateMachine m;
  m.Apply(Signal::kTitle);
  REQUIRE(m.Apply(Signal::kCrash)->to == State::kCrashed);
  REQUIRE_FALSE(m.Apply(Signal::kTitle));
  REQUIRE_FALSE(m.Apply(Signal::kResume));
  REQUIRE(m.Apply(Signal::kExit)->to == State::kExited);
  REQUIRE_FALSE(m.Apply(Signal::kExit));
}

TEST_CASE("telemetry: QueryHostInfo is populated", "[telemetry]") {
  auto h = xe::telemetry::QueryHostInfo();
  REQUIRE_FALSE(h.os.empty());
  REQUIRE_FALSE(h.os_version.empty());
  REQUIRE_FALSE(h.cpu.empty());
  REQUIRE(h.cpu_cores >= 1);
  REQUIRE(h.cpu_threads >= h.cpu_cores);
  REQUIRE(h.ram_mb > 0);
}

TEST_CASE("telemetry: PeakDb", "[telemetry]") {
  REQUIRE(*xe::telemetry::PeakDb(1.0f) == 0.0);
  REQUIRE(*xe::telemetry::PeakDb(0.5f) == -6.0);
  REQUIRE_FALSE(xe::telemetry::PeakDb(0.0f));
}

TEST_CASE("telemetry: FirstTime dedups per kind and key", "[telemetry]") {
  REQUIRE(xe::telemetry::FirstTime("t1", "a"));
  REQUIRE_FALSE(xe::telemetry::FirstTime("t1", "a"));
  REQUIRE(xe::telemetry::FirstTime("t2", "a"));
}

TEST_CASE("telemetry: disabled by default is a no-op", "[telemetry]") {
  REQUIRE_FALSE(xe::telemetry::enabled());
  xe::telemetry::Emit(xe::telemetry::Event("x"));  // no crash, no sink
  xe::telemetry::Count(xe::telemetry::Counter::kInputPolls);
}

TEST_CASE("telemetry: OpenFile fails cleanly on an unwritable path",
          "[telemetry]") {
  REQUIRE_FALSE(xe::telemetry::OpenFile(std::filesystem::temp_directory_path() /
                                        "no_such_dir_xe" / "t.jsonl"));
  REQUIRE_FALSE(xe::telemetry::enabled());
}

TEST_CASE(
    "telemetry: EmitNow is on disk before Shutdown and readable while open",
    "[telemetry]") {
  auto path =
      std::filesystem::temp_directory_path() / "xe_telemetry_test.jsonl";
  REQUIRE(xe::telemetry::OpenFile(path));
  xe::telemetry::EmitNow(xe::telemetry::Event("crash").Str("side", "guest"));
  std::ifstream in(path);  // second handle while the writer holds it
  std::string first;
  // OpenFile writes nothing itself; the emulator emits session_start.
  std::getline(in, first);
  REQUIRE(first.find(R"("type":"crash")") != std::string::npos);
  xe::telemetry::Shutdown();
  REQUIRE_FALSE(xe::telemetry::enabled());
}

TEST_CASE("telemetry: Shutdown writes exit and exited last", "[telemetry]") {
  using namespace xe::telemetry;
  auto path = std::filesystem::temp_directory_path() /
              "xe_telemetry_shutdown_test.jsonl";
  REQUIRE(OpenFile(path));
  Notify(Signal::kTitle);
  for (int i = 0; i < 100; ++i) {
    Emit(Event("queued").Int("i", i));
  }
  // Another thread keeps emitting through Shutdown, as the vblank and guest
  // threads do on the quick_exit path.
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> iterations{0};
  std::thread racer([&] {
    uint64_t vblank = 0;
    while (!stop.load()) {
      Emit(Event("race"));
      EmitNow(Event("race_now"));
      OnVblank(++vblank, 0);
      Notify(Signal::kTitle);
      iterations.fetch_add(1);
    }
  });
  while (iterations.load() < 100) {
    std::this_thread::yield();
  }
  Shutdown();
  uint64_t iterations_at_shutdown = iterations.load();
  while (iterations.load() < iterations_at_shutdown + 100) {
    std::this_thread::yield();
  }
  stop = true;
  racer.join();

  REQUIRE_FALSE(enabled());
  REQUIRE(CurrentState() == State::kStarting);

  std::ifstream in(path);
  std::vector<std::string> lines;
  for (std::string line; std::getline(in, line);) {
    lines.push_back(line);
  }
  REQUIRE(lines.size() >= 102);
  const std::string& exit_line = lines[lines.size() - 2];
  const std::string& last = lines.back();
  REQUIRE(exit_line.find(R"("type":"exit")") != std::string::npos);
  REQUIRE(last.find(R"("type":"state")") != std::string::npos);
  REQUIRE(last.find(R"("to":"exited")") != std::string::npos);
  REQUIRE(last.find(R"("state":"exited")") != std::string::npos);
  // Everything queued before Shutdown was written, before exit.
  size_t queued = 0;
  for (const std::string& line : lines) {
    queued += line.find(R"("type":"queued")") != std::string::npos;
  }
  REQUIRE(queued == 100);
}

#if XE_PLATFORM_WIN32
// External tools line telemetry up with their own clock (e.g. Python's
// time.perf_counter_ns, which reads QueryPerformanceCounter), so NowNs must
// share that timebase.
TEST_CASE("telemetry: NowNs shares the QPC timebase", "[telemetry]") {
  LARGE_INTEGER frequency, counter;
  QueryPerformanceFrequency(&frequency);
  uint64_t now_ns = xe::telemetry::NowNs();
  QueryPerformanceCounter(&counter);
  uint64_t ticks = uint64_t(counter.QuadPart);
  uint64_t freq = uint64_t(frequency.QuadPart);
  uint64_t qpc_ns = (ticks / freq) * 1'000'000'000ull +
                    (ticks % freq) * 1'000'000'000ull / freq;
  uint64_t diff = qpc_ns > now_ns ? qpc_ns - now_ns : now_ns - qpc_ns;
  REQUIRE(diff < 1'000'000ull);
}
#endif

// The exit line's reason after a session driven by session().
static std::string ExitReasonAfter(const std::function<void()>& session) {
  using namespace xe::telemetry;
  auto path = std::filesystem::temp_directory_path() /
              "xe_telemetry_exit_reason_test.jsonl";
  REQUIRE(OpenFile(path));
  session();
  Shutdown();
  std::ifstream in(path);
  std::vector<std::string> lines;
  for (std::string line; std::getline(in, line);) {
    lines.push_back(line);
  }
  REQUIRE(lines.size() >= 2);
  const std::string& exit_line = lines[lines.size() - 2];
  REQUIRE(exit_line.find(R"("type":"exit")") != std::string::npos);
  const std::string key = R"("reason":")";
  size_t begin = exit_line.find(key);
  REQUIRE(begin != std::string::npos);
  begin += key.size();
  return exit_line.substr(begin, exit_line.find('"', begin) - begin);
}

TEST_CASE("telemetry: exit reason precedence", "[telemetry]") {
  using namespace xe::telemetry;
  REQUIRE(ExitReasonAfter([] {}) == "shutdown");
  REQUIRE(ExitReasonAfter([] {
            Notify(Signal::kTitle);
            SetExitReason("title_exit");
          }) == "title_exit");
  // Closing a crashed title keeps crash.
  REQUIRE(ExitReasonAfter([] {
            Notify(Signal::kTitle);
            SetExitReason("crash");
            SetExitReason("title_exit");
          }) == "crash");
  // A new title after a title_exit starts over.
  REQUIRE(ExitReasonAfter([] {
            Notify(Signal::kTitle);
            SetExitReason("title_exit");
            Notify(Signal::kTitle);
          }) == "shutdown");
  REQUIRE(ExitReasonAfter([] {
            Notify(Signal::kTitle);
            SetExitReason("crash");
            Notify(Signal::kTitle);
          }) == "crash");
}

TEST_CASE("telemetry: paused until every pause source resumes", "[telemetry]") {
  using namespace xe::telemetry;
  auto path = std::filesystem::temp_directory_path() /
              "xe_telemetry_pause_sources_test.jsonl";
  REQUIRE(OpenFile(path));
  Notify(Signal::kTitle);
  REQUIRE(CurrentState() == State::kLoading);
  OnPause(PauseSource::kEmulator, true);
  REQUIRE(CurrentState() == State::kPaused);
  OnPause(PauseSource::kDebugger, true);
  OnPause(PauseSource::kDebugger, false);
  REQUIRE(CurrentState() == State::kPaused);
  OnPause(PauseSource::kEmulator, false);
  REQUIRE(CurrentState() == State::kLoading);
  // A title launched during a debugger break stays paused.
  OnPause(PauseSource::kDebugger, true);
  Notify(Signal::kTitle);
  REQUIRE(CurrentState() == State::kPaused);
  OnPause(PauseSource::kDebugger, false);
  REQUIRE(CurrentState() == State::kLoading);
  Shutdown();
}

}  // namespace xe::base::test
