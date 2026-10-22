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
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <thread>

#include "xenia/base/exception_handler.h"

#include "third_party/catch/include/catch.hpp"

namespace xe::base::test {

namespace {

// Runs child in a forked process with the default SIGSEGV/SIGBUS actions
// instead of Catch2's, and returns its wait status. Fails if it hangs.
template <typename F>
int RunChild(F child) {
  pid_t pid = fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    struct sigaction default_handler = {};
    default_handler.sa_handler = SIG_DFL;
    sigaction(SIGSEGV, &default_handler, nullptr);
    sigaction(SIGBUS, &default_handler, nullptr);
    struct rlimit no_core = {0, 0};
    setrlimit(RLIMIT_CORE, &no_core);
    child();
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
    FAIL("The child hung");
  }
  REQUIRE(waited == pid);
  return status;
}

bool ClaimNothing(Exception*, void*) { return false; }

void* MapNoAccessPage() {
  void* page = mmap(nullptr, size_t(getpagesize()), PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (page == MAP_FAILED) {
    _exit(100);
  }
  return page;
}

// Pages for the chaining test: the original handler owns the first, xenia's
// handler the second.
void* chain_original_page = nullptr;
void* chain_xenia_page = nullptr;

void ChainOriginalHandler(int, siginfo_t* info, void*) {
  if (info->si_addr != chain_original_page) {
    _exit(1);
  }
  mprotect(chain_original_page, size_t(getpagesize()), PROT_READ | PROT_WRITE);
}

bool ClaimXeniaPage(Exception* ex, void*) {
  if (ex->code() != Exception::Code::kAccessViolation ||
      ex->fault_address() != reinterpret_cast<uint64_t>(chain_xenia_page)) {
    return false;
  }
  mprotect(chain_xenia_page, size_t(getpagesize()), PROT_READ | PROT_WRITE);
  return true;
}

bool ExitOnAccessViolation(Exception* ex, void* expected_address) {
  if (ex->code() == Exception::Code::kAccessViolation &&
      ex->fault_address() == reinterpret_cast<uint64_t>(expected_address)) {
    _exit(42);
  }
  return false;
}

#if XE_PLATFORM_LINUX
// Maps a page of an empty file: touching it raises SIGBUS.
volatile int* MapPastEndOfFile() {
  int fd = memfd_create("xenia_exception_handler_test", 0);
  if (fd < 0) {
    _exit(101);
  }
  void* map = mmap(nullptr, size_t(getpagesize()), PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
  close(fd);
  if (map == MAP_FAILED) {
    _exit(102);
  }
  return static_cast<volatile int*>(map);
}
#endif  // XE_PLATFORM_LINUX

}  // namespace

// An unclaimed fault must terminate the process, not loop forever.
TEST_CASE("Unclaimed SIGSEGV terminates the process", "[exception_handler]") {
  int status = RunChild([]() {
    ExceptionHandler::Install(ClaimNothing, nullptr);
    volatile int* volatile null_pointer = nullptr;
    *null_pointer = 1;
  });
  REQUIRE(WIFSIGNALED(status));
  REQUIRE(WTERMSIG(status) == SIGSEGV);
}

TEST_CASE("Unclaimed SIGSEGV with an ignored action terminates the process",
          "[exception_handler]") {
  int status = RunChild([]() {
    struct sigaction ignore = {};
    ignore.sa_handler = SIG_IGN;
    sigaction(SIGSEGV, &ignore, nullptr);
    ExceptionHandler::Install(ClaimNothing, nullptr);
    volatile int* volatile null_pointer = nullptr;
    *null_pointer = 1;
  });
  REQUIRE(WIFSIGNALED(status));
  REQUIRE(WTERMSIG(status) == SIGSEGV);
}

TEST_CASE("Unclaimed SIGSEGV goes to the original handler, which stays chained",
          "[exception_handler]") {
  int status = RunChild([]() {
    chain_original_page = MapNoAccessPage();
    chain_xenia_page = MapNoAccessPage();
    struct sigaction original = {};
    original.sa_sigaction = ChainOriginalHandler;
    original.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &original, nullptr);
    ExceptionHandler::Install(ClaimXeniaPage, nullptr);
    *static_cast<volatile int*>(chain_original_page) = 1;
    *static_cast<volatile int*>(chain_xenia_page) = 1;
    _exit(0);
  });
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);
}

#if XE_PLATFORM_LINUX
TEST_CASE("SIGBUS reaches the installed handlers", "[exception_handler]") {
  int status = RunChild([]() {
    volatile int* map = MapPastEndOfFile();
    ExceptionHandler::Install(ExitOnAccessViolation, const_cast<int*>(map));
    *map = 1;
  });
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 42);
}

TEST_CASE("Unclaimed SIGBUS terminates the process", "[exception_handler]") {
  int status = RunChild([]() {
    volatile int* map = MapPastEndOfFile();
    ExceptionHandler::Install(ClaimNothing, nullptr);
    *map = 1;
  });
  REQUIRE(WIFSIGNALED(status));
  REQUIRE(WTERMSIG(status) == SIGBUS);
}
#endif  // XE_PLATFORM_LINUX

}  // namespace xe::base::test

#endif  // !XE_PLATFORM_WIN32
