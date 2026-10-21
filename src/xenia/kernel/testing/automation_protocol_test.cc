/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/automation_protocol.h"

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace automation {
namespace test {

namespace {

Request Parsed(std::string_view line) {
  ParseResult result = ParseRequest(line);
  REQUIRE(result.request.has_value());
  return result.request.value_or(Request());
}

rapidjson::Document Json(const std::string& line) {
  REQUIRE(!line.empty());
  REQUIRE(line.back() == '\n');
  rapidjson::Document document;
  document.Parse(line.data(), line.size() - 1);
  REQUIRE(!document.HasParseError());
  return document;
}

Context TestContext() {
  Context context;
  context.token = "secret";
  context.build = {"main", "abc123", "2026-10-20"};
  context.snapshot = {true, 0x4D5307D5, "Title"};
  return context;
}

}  // namespace

TEST_CASE("Lines split on newlines, across appends", "[automation]") {
  LineSplitter splitter;
  splitter.Append("{\"op\":\"hel");
  REQUIRE(!splitter.NextLine());
  splitter.Append("lo\"}\r\n{\"op\":\"state\"}\n{\"op\"");
  REQUIRE(splitter.NextLine() == "{\"op\":\"hello\"}");
  REQUIRE(splitter.NextLine() == "{\"op\":\"state\"}");
  REQUIRE(!splitter.NextLine());
  REQUIRE(!splitter.overflowed());
}

TEST_CASE("An unterminated line too long overflows", "[automation]") {
  LineSplitter splitter;
  splitter.Append(std::string(kMaxLineLength + 1, 'x'));
  REQUIRE(!splitter.NextLine());
  REQUIRE(splitter.overflowed());
}

TEST_CASE("Requests parse op, id and hello fields", "[automation]") {
  Request hello =
      Parsed(R"({"op":"hello","id":7,"protocol":1,"token":"secret"})");
  REQUIRE(hello.op == "hello");
  REQUIRE(hello.id == "7");
  REQUIRE(hello.protocol == 1u);
  REQUIRE(hello.token == "secret");
  REQUIRE(Parsed(R"({"op":"state","id":"a"})").id == "\"a\"");
  REQUIRE(Parsed(R"({"op":"state"})").id.empty());
}

TEST_CASE("Malformed requests are refused", "[automation]") {
  REQUIRE(ParseRequest("not json").error == "not a JSON object");
  REQUIRE(ParseRequest("[1]").error == "not a JSON object");
  REQUIRE(ParseRequest(R"({"id":3})").error == "missing op");
  REQUIRE(ParseRequest(R"({"id":3})").id == "3");
  REQUIRE(ParseRequest(R"({"op":"x","id":{}})").error ==
          "id must be a string or a number");
  REQUIRE(ParseRequest(R"({"op":"hello","token":1})").error ==
          "token must be a string");
}

TEST_CASE("Tokens compare whole", "[automation]") {
  REQUIRE(TokensEqual("secret", "secret"));
  REQUIRE(!TokensEqual("secret", "secreT"));
  REQUIRE(!TokensEqual("secret", "secret2"));
  REQUIRE(!TokensEqual("", "secret"));
  std::array<uint8_t, 16> bytes{};
  bytes[0] = 0xAB;
  bytes[15] = 0x01;
  REQUIRE(TokenFromBytes(bytes) == "ab000000000000000000000000000001");
}

TEST_CASE("Hello comes first", "[automation]") {
  Session session;
  Decision decision =
      Decide(session, Parsed(R"({"op":"state","id":1})"), TestContext());
  REQUIRE(decision.action == Action::kClose);
  REQUIRE(Json(decision.reply)["error"] == "hello first");
  REQUIRE(Json(decision.reply)["re"] == 1);
  REQUIRE(session.role == Role::kNone);
}

TEST_CASE("Hello checks the protocol and the token", "[automation]") {
  Session session;
  Decision wrong_protocol =
      Decide(session, Parsed(R"({"op":"hello","protocol":2,"token":"secret"})"),
             TestContext());
  REQUIRE(wrong_protocol.action == Action::kClose);
  REQUIRE(Json(wrong_protocol.reply)["protocol"] == kProtocolVersion);
  Decision wrong_token =
      Decide(session, Parsed(R"({"op":"hello","protocol":1,"token":"guess"})"),
             TestContext());
  REQUIRE(wrong_token.action == Action::kClose);
  REQUIRE(Json(wrong_token.reply)["error"] == "bad token");
  REQUIRE(session.role == Role::kNone);
}

TEST_CASE("Hello describes the build and the state", "[automation]") {
  Session session;
  Decision decision = Decide(
      session, Parsed(R"({"op":"hello","id":1,"protocol":1,"token":"secret"})"),
      TestContext());
  REQUIRE(decision.action == Action::kNone);
  REQUIRE(session.role == Role::kController);
  rapidjson::Document reply = Json(decision.reply);
  REQUIRE(reply["ok"] == true);
  REQUIRE(reply["role"] == "controller");
  REQUIRE(reply["xenia"]["commit"] == "abc123");
  REQUIRE(reply["capabilities"].IsArray());
  REQUIRE(reply["state"]["title_id"] == "4D5307D5");
  REQUIRE(reply["state"]["title"] == "Title");
}

TEST_CASE("Later clients observe and can't stop", "[automation]") {
  Context context = TestContext();
  context.controller_free = false;
  Session session;
  Decide(session, Parsed(R"({"op":"hello","protocol":1,"token":"secret"})"),
         context);
  REQUIRE(session.role == Role::kObserver);
  Decision stop = Decide(session, Parsed(R"({"op":"stop"})"), context);
  REQUIRE(stop.action == Action::kNone);
  REQUIRE(Json(stop.reply)["error"] == "needs the controller role");
}

TEST_CASE("The controller stops the run", "[automation]") {
  Session session{Role::kController};
  Decision stop =
      Decide(session, Parsed(R"({"op":"stop","id":9})"), TestContext());
  REQUIRE(stop.action == Action::kStop);
  REQUIRE(Json(stop.reply)["ok"] == true);
  REQUIRE(Json(ExitEvent(0))["type"] == "exit");
}

TEST_CASE("State and unknown ops", "[automation]") {
  Session session{Role::kObserver};
  Context context = TestContext();
  context.snapshot = {};
  rapidjson::Document state =
      Json(Decide(session, Parsed(R"({"op":"state"})"), context).reply);
  REQUIRE(state["state"]["title_open"] == false);
  REQUIRE(!state["state"].HasMember("title_id"));
  REQUIRE(Json(Decide(session, Parsed(R"({"op":"dance"})"), context)
                   .reply)["error"] == "unknown op");
}

}  // namespace test
}  // namespace automation
}  // namespace xe
