/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <atomic>
#include <thread>
#include <vector>

#include "xenia/base/function_queue.h"

#include "third_party/catch/include/catch.hpp"

namespace xe::base::test {

TEST_CASE("FunctionQueue runs concurrent pushes once each, in order",
          "[function_queue]") {
  constexpr int kProducers = 8;
  constexpr int kPerProducer = 20000;

  FunctionQueue queue;
  // Only the consumer thread runs the functions, so plain ints suffice.
  std::vector<int> next_expected(kProducers, 0);
  int out_of_order = 0;
  int ran = 0;
  std::atomic<int> producers_done = 0;

  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&, p]() {
      for (int i = 0; i < kPerProducer; ++i) {
        queue.Push([&, p, i]() {
          if (next_expected[p] != i) {
            ++out_of_order;
          }
          next_expected[p] = i + 1;
          ++ran;
        });
      }
      ++producers_done;
    });
  }

  while (producers_done.load() < kProducers || !queue.empty()) {
    queue.RunAll();
  }
  for (auto& producer : producers) {
    producer.join();
  }
  queue.RunAll();

  CHECK(out_of_order == 0);
  CHECK(ran == kProducers * kPerProducer);
  for (int p = 0; p < kProducers; ++p) {
    CHECK(next_expected[p] == kPerProducer);
  }
}

TEST_CASE("FunctionQueue keeps the rest of the queue ahead of new pushes",
          "[function_queue]") {
  FunctionQueue queue;
  std::vector<int> order;
  queue.Push([&]() {
    order.push_back(1);
    CHECK_FALSE(queue.empty());
    queue.Push([&]() { order.push_back(3); });
  });
  queue.Push([&]() { order.push_back(2); });
  queue.RunAll();
  CHECK(order == std::vector<int>{1, 2, 3});
  CHECK(queue.empty());
}

}  // namespace xe::base::test
