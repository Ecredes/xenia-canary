/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/string_buffer.h"

#include <cstdarg>
#include <string>

#include "third_party/catch/include/catch.hpp"

namespace xe::base::test {

namespace {
void AppendVarargsHelper(StringBuffer& buffer, const char* format, ...) {
  va_list args;
  va_start(args, format);
  buffer.AppendVarargs(format, args);
  va_end(args);
}
}  // namespace

TEST_CASE("StringBuffer AppendVarargs formats its arguments",
          "[string_buffer]") {
  StringBuffer buffer;
  AppendVarargsHelper(buffer, "frame %d size %d name %s", 7, 1024, "xma");
  REQUIRE(buffer.to_string() == "frame 7 size 1024 name xma");

  // Appending after existing content formats into the remaining space.
  AppendVarargsHelper(buffer, " | %u", 42u);
  REQUIRE(buffer.to_string() == "frame 7 size 1024 name xma | 42");
}

}  // namespace xe::base::test
