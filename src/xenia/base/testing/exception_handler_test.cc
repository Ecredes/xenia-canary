/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/platform.h"

#if !XE_PLATFORM_WIN32

#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <thread>

#include "xenia/base/exception_handler.h"

#include "third_party/catch/include/catch.hpp"

namespace xe::base::test {

// A fault that no installed handler claims is a host crash. The signal
// handler used to return without doing anything, so the faulting instruction
// ran again and faulted forever, and the process hung at 100% CPU instead of
// crashing. It must terminate with the original signal.
TEST_CASE("Unclaimed SIGSEGV terminates the process", "[exception_handler]") {
  pid_t pid = fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    // Child: start from the default action (not Catch2's handler) and don't
    // write a core file.
    struct sigaction default_handler = {};
    default_handler.sa_handler = SIG_DFL;
    sigaction(SIGSEGV, &default_handler, nullptr);
    struct rlimit no_core = {0, 0};
    setrlimit(RLIMIT_CORE, &no_core);
    ExceptionHandler::Install([](Exception*, void*) { return false; }, nullptr);
    volatile int* volatile null_pointer = nullptr;
    *null_pointer = 1;
    _exit(0);
  }

  int status = 0;
  pid_t waited = 0;
  for (int i = 0; i < 1000 && waited == 0; ++i) {
    waited = waitpid(pid, &status, WNOHANG);
    if (waited == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  if (waited == 0) {
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    FAIL("The child hung on an unclaimed SIGSEGV");
  }
  REQUIRE(waited == pid);
  REQUIRE(WIFSIGNALED(status));
  REQUIRE(WTERMSIG(status) == SIGSEGV);
}

}  // namespace xe::base::test

#endif  // !XE_PLATFORM_WIN32
