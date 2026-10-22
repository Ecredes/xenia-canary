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

TEST_CASE("The controller pauses, resumes and advances", "[automation]") {
  Session session{Role::kController};
  Context context = TestContext();
  REQUIRE(Decide(session, Parsed(R"({"op":"pause"})"), context).action ==
          Action::kPause);
  REQUIRE(Decide(session, Parsed(R"({"op":"resume"})"), context).action ==
          Action::kResume);
  Decision advance =
      Decide(session, Parsed(R"({"op":"advance","vblanks":30})"), context);
  REQUIRE(advance.action == Action::kAdvance);
  REQUIRE(advance.vblanks == 30);
  REQUIRE(advance.reply.empty());
  for (const char* line :
       {R"({"op":"advance"})", R"({"op":"advance","vblanks":0})",
        R"({"op":"advance","vblanks":3601})"}) {
    Decision refused = Decide(session, Parsed(line), context);
    REQUIRE(refused.action == Action::kNone);
    REQUIRE(Json(refused.reply)["error"] == "vblanks must be from 1 to 3600");
  }
  rapidjson::Document reply = Json(PauseReply("7", true, 120, 61));
  REQUIRE(reply["re"] == 7);
  REQUIRE(reply["paused"] == true);
  REQUIRE(reply["vblank"] == 120);
  REQUIRE(reply["swap"] == 61);
}

TEST_CASE("Observers can't pause", "[automation]") {
  Session session{Role::kObserver};
  for (const char* line : {R"({"op":"pause"})", R"({"op":"resume"})",
                           R"({"op":"advance","vblanks":1})"}) {
    Decision refused = Decide(session, Parsed(line), TestContext());
    REQUIRE(refused.action == Action::kNone);
    REQUIRE(Json(refused.reply)["error"] == "needs the controller role");
  }
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

TEST_CASE("Subscribing replaces the streams", "[automation]") {
  Session session{Role::kObserver};
  Context context = TestContext();
  Decision first =
      Decide(session, Parsed(R"({"op":"subscribe","streams":["telemetry"]})"),
             context);
  REQUIRE(session.telemetry);
  REQUIRE(!session.dialogs);
  REQUIRE(Json(first.reply)["streams"][0] == "telemetry");
  Decision both =
      Decide(session,
             Parsed(R"({"op":"subscribe","streams":["dialogs","telemetry"]})"),
             context);
  REQUIRE(session.telemetry);
  REQUIRE(session.dialogs);
  REQUIRE(Json(both.reply)["streams"].Size() == 2);
  Decision none =
      Decide(session, Parsed(R"({"op":"subscribe","streams":[]})"), context);
  REQUIRE(none.action == Action::kNone);
  REQUIRE(!session.telemetry);
  REQUIRE(!session.dialogs);
}

TEST_CASE("Subscribing needs known streams", "[automation]") {
  Session session{Role::kObserver};
  REQUIRE(Json(Decide(session, Parsed(R"({"op":"subscribe"})"), TestContext())
                   .reply)["error"] == "missing streams");
  REQUIRE(
      Json(Decide(session, Parsed(R"({"op":"subscribe","streams":["video"]})"),
                  TestContext())
               .reply)["error"] == "unknown stream video");
  REQUIRE(!session.telemetry);
  REQUIRE(ParseRequest(R"({"op":"subscribe","streams":"telemetry"})").error ==
          "streams must be a list");
  REQUIRE(ParseRequest(R"({"op":"subscribe","streams":[1]})").error ==
          "streams must be a list of names");
}

TEST_CASE("The session_start line is recognized", "[automation]") {
  REQUIRE(IsSessionStart(R"({"type":"session_start","vblank":0})"));
  REQUIRE(!IsSessionStart(R"({"type":"state","vblank":0})"));
}

namespace {

std::string AnswerError(const DialogRequest& request, std::string_view json) {
  std::string error;
  REQUIRE(!ParseDialogAnswer(request, json, &error));
  return error;
}

DialogAnswer Answered(const DialogRequest& request, std::string_view json) {
  std::string error;
  std::optional<DialogAnswer> answer = ParseDialogAnswer(request, json, &error);
  INFO(error);
  REQUIRE(answer.has_value());
  return answer.value_or(DialogAnswer());
}

}  // namespace

TEST_CASE("Only the controller answers dialogs", "[automation]") {
  Session observer{Role::kObserver};
  REQUIRE(Json(Decide(observer,
                      Parsed(R"({"op":"dialog_answer","dialog":1,)"
                             R"("answer":{"button":0}})"),
                      TestContext())
                   .reply)["error"] == "needs the controller role");
  Session controller{Role::kController};
  REQUIRE(Decide(controller,
                 Parsed(R"({"op":"dialog_answer","dialog":1,)"
                        R"("answer":{"button":0}})"),
                 TestContext())
              .action == Action::kAnswerDialog);
  REQUIRE(Json(Decide(controller, Parsed(R"({"op":"dialog_answer"})"),
                      TestContext())
                   .reply)["error"] == "missing dialog or answer");
  REQUIRE(ParseRequest(R"({"op":"dialog_answer","answer":1})").error ==
          "answer must be an object");
}

TEST_CASE("Dialog events carry the request and the default", "[automation]") {
  ChoiceRequest choice{"Save", "Overwrite?", {"Yes", "No"}, 1, 1};
  std::string line = DialogEvent(7, choice, PolicyAnswer(choice), 30000)
                         .Finish(10, 2, 99, telemetry::State::kSystemUi);
  rapidjson::Document event = Json(line + "\n");
  REQUIRE(event["type"] == "dialog");
  REQUIRE(event["vblank"] == 10);
  REQUIRE(event["dialog"] == 7);
  REQUIRE(event["kind"] == "choice");
  REQUIRE(event["buttons"][1] == "No");
  REQUIRE(event["default"]["button"] == 1);
  REQUIRE(event["timeout_ms"] == 30000);

  SignInRequest sign_in{2, 0, {{1, 0xE000000012345678ull}}};
  rapidjson::Document signed_in =
      Json(DialogEvent(8, sign_in, PolicyAnswer(sign_in), 30000)
               .Finish(0, 0, 0, telemetry::State::kRunning) +
           "\n");
  REQUIRE(signed_in["signed_in"][0]["xuid"] == "E000000012345678");
  REQUIRE(signed_in["default"]["profiles"][0]["slot"] == 1);

  rapidjson::Document closed =
      Json(DialogClosedEvent(7, "timeout")
               .Finish(0, 0, 0, telemetry::State::kRunning) +
           "\n");
  REQUIRE(closed["by"] == "timeout");
}

TEST_CASE("Choice and text answers are checked", "[automation]") {
  ChoiceRequest choice{"", "", {"A", "B"}, 0, 0};
  REQUIRE(std::get<0>(Answered(choice, R"({"button":1})")) == 1u);
  REQUIRE(AnswerError(choice, R"({"button":2})") ==
          "button must be one of the buttons' indexes");
  REQUIRE(AnswerError(choice, R"({"cancel":true})") ==
          "a choice can't be cancelled");
  TextRequest text{"", "", "", 4};
  REQUIRE(std::get<1>(Answered(text, R"({"text":"abcd"})")) == "abcd");
  REQUIRE(!std::get<1>(Answered(text, R"({"cancel":true})")));
  REQUIRE(AnswerError(text, R"({"text":"abcde"})") ==
          "text is longer than max_length");
}

TEST_CASE("Sign-in, pass code and disc answers are checked", "[automation]") {
  SignInRequest sign_in{1, 0, {}};
  SignInAnswer chosen = std::get<2>(Answered(
      sign_in, R"({"profiles":[{"slot":2,"xuid":"e0000000000000aa"}]})"));
  REQUIRE(chosen.value_or(std::map<uint8_t, uint64_t>()) ==
          std::map<uint8_t, uint64_t>{{2, 0xE0000000000000AAull}});
  REQUIRE(AnswerError(sign_in, R"({"profiles":[{"slot":4,"xuid":"1"}]})") ==
          "each profile needs a slot below 4 and a hex xuid");
  REQUIRE(AnswerError(sign_in, R"({"profiles":[{"slot":0,"xuid":"1"},)"
                               R"({"slot":1,"xuid":"2"}]})") ==
          "more profiles than users_needed");
  PasscodeRequest passcode{"", "", 4};
  REQUIRE(std::get<3>(Answered(passcode, R"({"keys":[1,2,3,4]})")) ==
          std::vector<uint16_t>{1, 2, 3, 4});
  REQUIRE(AnswerError(passcode, R"({"keys":[1,2]})") ==
          "keys must be a list of length keys, or cancel true");
  DiscRequest disc{"Insert disc 2"};
  REQUIRE(std::get<4>(Answered(disc, R"({"path":"d2.iso"})")) == "d2.iso");
  REQUIRE(std::get<4>(Answered(disc, R"({"cancel":true})")).empty());
}

TEST_CASE("Answering an open dialog", "[automation]") {
  Request request = Parsed(R"({"op":"dialog_answer","id":5,"dialog":3,)"
                           R"("answer":{"button":0}})");
  DialogAnswerDecision missing = DecideDialogAnswer(request, nullptr);
  REQUIRE(!missing.answer);
  REQUIRE(Json(missing.reply)["error"] == "no such open dialog");
  DialogRequest pending = ChoiceRequest{"", "", {"OK"}, 0, 0};
  DialogAnswerDecision answered = DecideDialogAnswer(request, &pending);
  REQUIRE(answered.answer);
  REQUIRE(Json(answered.reply)["re"] == 5);
}

TEST_CASE("Pad ops become checked commands", "[automation]") {
  using hid::remote::Command;
  Session controller{Role::kController};
  Decision queue = Decide(
      controller,
      Parsed(R"({"op":"pad_queue","id":1,"user":2,"input":9,"frames":3,)"
             R"("pad":{"buttons":4096,"lt":255,"lx":-32768,"ry":32767}})"),
      TestContext());
  REQUIRE(queue.action == Action::kPad);
  REQUIRE(queue.pad);
  Command command = queue.pad.value_or(Command());
  REQUIRE(command.type == Command::Type::kQueue);
  REQUIRE(command.user == 2);
  REQUIRE(command.id == 9);
  REQUIRE(command.frames == 3);
  REQUIRE(command.state.buttons == 0x1000);
  REQUIRE(command.state.left_trigger == 255);
  REQUIRE(command.state.thumb_lx == -32768);
  REQUIRE(command.state.thumb_ry == 32767);
  REQUIRE(command.state.right_trigger == 0);

  Command connect =
      Decide(controller,
             Parsed(R"({"op":"pad_connect","user":0,"connected":true})"),
             TestContext())
          .pad.value_or(Command());
  REQUIRE(connect.type == Command::Type::kConnect);
  REQUIRE(connect.value == 1);
  REQUIRE(Decide(controller, Parsed(R"({"op":"pad_clear","user":3})"),
                 TestContext())
              .pad.value_or(Command())
              .type == Command::Type::kClear);
  REQUIRE(Json(PadReply("1", ""))["ok"] == true);
  REQUIRE(Json(PadReply("1", "queue full"))["error"] == "queue full");
}

TEST_CASE("Pad ops are checked", "[automation]") {
  Session controller{Role::kController};
  auto error = [&controller](std::string_view line) {
    Decision decision = Decide(controller, Parsed(line), TestContext());
    REQUIRE(decision.action == Action::kNone);
    return std::string(Json(decision.reply)["error"].GetString());
  };
  REQUIRE(error(R"({"op":"pad_set","user":4,"pad":{}})") ==
          "user must be from 0 to 3");
  REQUIRE(error(R"({"op":"pad_set","user":0})") == "missing pad");
  REQUIRE(error(R"({"op":"pad_set","user":0,"pad":{"lt":256}})") ==
          "pad lt must be from 0 to 255");
  REQUIRE(error(R"({"op":"pad_set","user":0,"pad":{"lx":32768}})") ==
          "pad lx must be from -32768 to 32767");
  REQUIRE(error(R"({"op":"pad_queue","user":0,"frames":1,"pad":{}})") ==
          "input must be a positive number");
  REQUIRE(error(R"({"op":"pad_queue","user":0,"input":1,"frames":3601,)"
                R"("pad":{}})") == "frames must be from 1 to 3600");
  REQUIRE(error(R"({"op":"pad_connect","user":0})") == "missing connected");
  REQUIRE(error(R"({"op":"pad_dance","user":0})") == "unknown op");
  Session observer{Role::kObserver};
  REQUIRE(Json(Decide(observer, Parsed(R"({"op":"pad_clear","user":0})"),
                      TestContext())
                   .reply)["error"] == "needs the controller role");
}

TEST_CASE("Pad events name the user, input and vblank", "[automation]") {
  hid::remote::PadEvent seen{hid::remote::PadEvent::Kind::kSeen, 1, 9, 120};
  rapidjson::Document event = Json(
      PadEvent(seen).Finish(121, 30, 5, telemetry::State::kRunning) + "\n");
  REQUIRE(event["type"] == "pad_seen");
  REQUIRE(event["user"] == 1);
  REQUIRE(event["input"] == 9);
  REQUIRE(event["at_vblank"] == 120);
  REQUIRE(event["vblank"] == 121);
}

TEST_CASE("Frame ops take a format, width and quality", "[automation]") {
  Session observer{Role::kObserver};
  Decision decision = Decide(
      observer,
      Parsed(R"({"op":"frame","format":"png","max_width":320,"quality":50})"),
      TestContext());
  REQUIRE(decision.action == Action::kFrame);
  REQUIRE(decision.frame_options == FrameOptions{FrameFormat::kPng, 320, 50});
  decision = Decide(observer, Parsed(R"({"op":"frame"})"), TestContext());
  REQUIRE(decision.frame_options == FrameOptions{});
  auto error = [&observer](std::string_view line) {
    Decision decision = Decide(observer, Parsed(line), TestContext());
    REQUIRE(decision.action == Action::kNone);
    return std::string(Json(decision.reply)["error"].GetString());
  };
  REQUIRE(error(R"({"op":"frame","format":"bmp"})") ==
          "format must be jpeg or png");
  REQUIRE(error(R"({"op":"frame","max_width":-1})") ==
          "max_width must be from 0 to 8192");
  REQUIRE(error(R"({"op":"frame","quality":0})") ==
          "quality must be from 1 to 100");
  REQUIRE(error(R"({"op":"frames_watch"})") == "fps must be from 0 to 60");
  REQUIRE(error(R"({"op":"frames_watch","fps":61})") ==
          "fps must be from 0 to 60");
}

TEST_CASE("Frames watches start and stop", "[automation]") {
  Session observer{Role::kObserver};
  REQUIRE(Json(Decide(observer,
                      Parsed(R"({"op":"frames_watch","fps":10,)"
                             R"("max_width":640})"),
                      TestContext())
                   .reply)["ok"] == true);
  FrameWatch watch = observer.frames.value_or(FrameWatch());
  REQUIRE(watch.fps == 10);
  REQUIRE(watch.options.max_width == 640);
  Decide(observer, Parsed(R"({"op":"frames_watch","fps":0})"), TestContext());
  REQUIRE(!observer.frames);
}

TEST_CASE("Watched frames keep their rate and need a new swap",
          "[automation]") {
  FrameWatch watch;
  watch.fps = 10;
  REQUIRE(FrameDue(watch, 0, 1));
  AdvanceWatch(watch, 0, 1);
  // The same swap again, or before the interval: not due.
  REQUIRE(!FrameDue(watch, 200000000, 1));
  REQUIRE(!FrameDue(watch, 99999999, 2));
  REQUIRE(FrameDue(watch, 105000000, 2));
  // Taken late, the next stays on the 100 ms grid.
  AdvanceWatch(watch, 105000000, 2);
  REQUIRE(watch.next_ns == 200000000);
  // After a stall, one interval from now rather than a burst.
  AdvanceWatch(watch, 950000000, 3);
  REQUIRE(watch.next_ns == 1050000000);
}

TEST_CASE("Base64 pads to whole quads", "[automation]") {
  auto encode = [](std::string_view text) {
    return Base64(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  };
  REQUIRE(encode("") == "");
  REQUIRE(encode("f") == "Zg==");
  REQUIRE(encode("fo") == "Zm8=");
  REQUIRE(encode("foo") == "Zm9v");
  REQUIRE(encode("foobar") == "Zm9vYmFy");
  const uint8_t high[] = {0xFF, 0xFE};
  REQUIRE(Base64(high, 2) == "//4=");
}

TEST_CASE("Frames scale down by averaging", "[automation]") {
  // 4x2 RGBX with a row stride of 20: left half 0, right half 200.
  std::vector<uint8_t> rgbx(20 * 2, 0xAA);
  for (uint32_t y = 0; y < 2; ++y) {
    for (uint32_t x = 0; x < 4; ++x) {
      uint8_t value = x < 2 ? 0 : 200;
      for (uint32_t c = 0; c < 3; ++c) {
        rgbx[y * 20 + x * 4 + c] = uint8_t(value + c);
      }
    }
  }
  RgbImage same = ScaleToWidth(rgbx.data(), 4, 2, 20, 0);
  REQUIRE(same.width == 4);
  REQUIRE(same.height == 2);
  REQUIRE(same.pixels[3 * 3 + 2] == 202);
  RgbImage half = ScaleToWidth(rgbx.data(), 4, 2, 20, 2);
  REQUIRE(half.width == 2);
  REQUIRE(half.height == 1);
  REQUIRE(half.pixels == std::vector<uint8_t>{0, 1, 2, 200, 201, 202});
  RgbImage odd = ScaleToWidth(rgbx.data(), 4, 2, 20, 3);
  REQUIRE(odd.width == 3);
  REQUIRE(odd.height == 2);
  // The middle pixel covers one dark and one light column.
  REQUIRE(odd.pixels[3] == 100);
  REQUIRE(ScaleToWidth(rgbx.data(), 0, 2, 20, 0).pixels.empty());
}

TEST_CASE("Frame times summarize as a review would", "[automation]") {
  // 99 frames of 10 ms and one of 50 ms.
  std::vector<double> ms(99, 10.0);
  ms.insert(ms.begin() + 40, 50.0);
  FrameTimeSummary summary = SummarizeFrameTimes(ms);
  REQUIRE(summary.frames == 100);
  REQUIRE(Round2(summary.avg_fps) == 96.15);
  // The slowest 1% (and 0.1%) is the one 50 ms frame.
  REQUIRE(summary.low_1_fps == 20.0);
  REQUIRE(summary.low_0_1_fps == 20.0);
  REQUIRE(summary.p50_ms == 10.0);
  REQUIRE(summary.p99_ms == 10.0);
  REQUIRE(summary.max_ms == 50.0);
  // Over 1000 frames the 1% low averages the slowest 10.
  std::vector<double> many(990, 10.0);
  many.insert(many.end(), 10, 40.0);
  REQUIRE(SummarizeFrameTimes(many).low_1_fps == 25.0);
  REQUIRE(SummarizeFrameTimes({}).frames == 0);

  std::vector<uint64_t> compiles(100, 0);
  compiles[40] = 2;
  Stutters stutters = CountStutters(ms, compiles);
  REQUIRE(stutters.count == 1);
  REQUIRE(stutters.with_compiles == 1);
  compiles[40] = 0;
  REQUIRE(CountStutters(ms, compiles).with_compiles == 0);
}

TEST_CASE("Benches gather frame_stats until stopped", "[automation]") {
  Session observer{Role::kObserver};
  REQUIRE(Json(Decide(observer, Parsed(R"({"op":"bench_stop"})"), TestContext())
                   .reply)["error"] == "no bench running");
  Decide(observer, Parsed(R"({"op":"bench_start"})"), TestContext());
  REQUIRE(observer.bench.has_value());
  // Gathered as the channel would, then handed to the session.
  BenchSamples samples;
  AddFrameStats(samples,
                R"({"type":"frame_stats","vblank":60,"swap":30,)"
                R"("ns":1000000000,"state":"running","window_vblanks":60,)"
                R"("swaps":30,"swap_ms":[33.3,33.4],"swap_compiles":[0,0],)"
                R"("presents":0,"present_ms":[],"present_latency_ms":[]})");
  AddFrameStats(samples, R"({"type":"state","vblank":61})");
  AddFrameStats(samples,
                R"({"type":"frame_stats","vblank":120,"swap":59,)"
                R"("ns":2000000000,"state":"running","window_vblanks":60,)"
                R"("swaps":29,"swap_ms":[100.0],"swap_compiles":[4],)"
                R"("presents":2,"present_ms":[16.7],)"
                R"("present_latency_ms":[3.5,4.5]})");
  REQUIRE(samples.windows == 2);
  REQUIRE(samples.swap_ms == std::vector<double>{33.3, 33.4, 100.0});
  observer.bench = samples;

  rapidjson::Document bench =
      Json(Decide(observer, Parsed(R"({"op":"bench_stop","id":3,"raw":true})"),
                  TestContext())
               .reply);
  REQUIRE(!observer.bench);
  REQUIRE(bench["re"] == 3);
  REQUIRE(bench["windows"] == 2);
  REQUIRE(bench["seconds"] == 1.0);
  REQUIRE(bench["vblank_hz"] == 60.0);
  REQUIRE(bench["swaps"]["frames"] == 3);
  REQUIRE(bench["swaps"]["max_ms"] == 100.0);
  REQUIRE(bench["stutters"] == 1);
  REQUIRE(bench["stutters_with_compiles"] == 1);
  REQUIRE(bench["compiles"] == 4);
  REQUIRE(bench["presents"]["frames"] == 1);
  REQUIRE(bench["present_latency_p50_ms"] == 3.5);
  REQUIRE(bench["present_latency_p99_ms"] == 4.5);
  REQUIRE(bench["swap_ms"].Size() == 3);

  // Without presents (headless) or raw, those are left out.
  Decide(observer, Parsed(R"({"op":"bench_start"})"), TestContext());
  bench = Json(
      Decide(observer, Parsed(R"({"op":"bench_stop"})"), TestContext()).reply);
  REQUIRE(bench["windows"] == 0);
  REQUIRE(!bench.HasMember("presents"));
  REQUIRE(!bench.HasMember("swap_ms"));
  REQUIRE(!bench.HasMember("vblank_hz"));
}

TEST_CASE("Frames carry the capture's counters", "[automation]") {
  EncodedFrame frame;
  frame.format = FrameFormat::kPng;
  frame.width = 2;
  frame.height = 1;
  frame.vblank = 70;
  frame.swap = 35;
  frame.ns = 123;
  frame.state = telemetry::State::kRunning;
  frame.bytes = {'f', 'o', 'o'};
  rapidjson::Document reply = Json(FrameReply("7", frame));
  REQUIRE(reply["re"] == 7);
  REQUIRE(reply["ok"] == true);
  REQUIRE(reply["format"] == "png");
  REQUIRE(reply["swap"] == 35);
  REQUIRE(reply["data"] == "Zm9v");
  rapidjson::Document event = Json(FrameEventLine(frame));
  REQUIRE(event["type"] == "frame");
  REQUIRE(event["vblank"] == 70);
  REQUIRE(event["swap"] == 35);
  REQUIRE(event["ns"] == 123);
  REQUIRE(event["state"] == "running");
  REQUIRE(event["width"] == 2);
  REQUIRE(event["data"] == "Zm9v");
}

TEST_CASE("Video starts with a port, an even size and a rate", "[automation]") {
  Session observer{Role::kObserver};
  Decision decision = Decide(observer,
                             Parsed(R"({"op":"video_start","port":5000,)"
                                    R"("width":1280,"height":720,"fps":60})"),
                             TestContext());
  REQUIRE(decision.action == Action::kVideoStart);
  REQUIRE(decision.video_options.port == 5000);
  REQUIRE(decision.video_options.width == 1280);
  REQUIRE(decision.video_options.height == 720);
  REQUIRE(decision.video_options.fps == 60);
  REQUIRE(Decide(observer, Parsed(R"({"op":"video_stop"})"), TestContext())
              .action == Action::kVideoStop);
  auto error = [&observer](std::string_view line) {
    Decision decision = Decide(observer, Parsed(line), TestContext());
    REQUIRE(decision.action == Action::kNone);
    return std::string(Json(decision.reply)["error"].GetString());
  };
  REQUIRE(error(R"({"op":"video_start","width":640,"height":360,"fps":30})") ==
          "port must be from 1024 to 65535");
  REQUIRE(error(R"({"op":"video_start","port":80,"width":640,"height":360,)"
                R"("fps":30})") == "port must be from 1024 to 65535");
  REQUIRE(error(R"({"op":"video_start","port":5000,"width":641,)"
                R"("height":360,"fps":30})") ==
          "width and height must be even, from 16 to 8192");
  REQUIRE(error(R"({"op":"video_start","port":5000,"width":640,"fps":30})") ==
          "width and height must be even, from 16 to 8192");
  REQUIRE(error(R"({"op":"video_start","port":5000,"width":640,)"
                R"("height":360,"fps":61})") == "fps must be from 1 to 60");
}

TEST_CASE("Video keeps a constant rate, repeating and dropping",
          "[automation]") {
  VideoClock clock;
  clock.fps = 10;
  clock.start_ns = 1000;
  REQUIRE(!NextVideoStep(clock, 999, 1, 5).send);
  // The first tick captures.
  VideoStep step = NextVideoStep(clock, 1000, 1, 5);
  REQUIRE(step.send);
  REQUIRE(step.capture);
  REQUIRE(step.repeats == 0);
  REQUIRE(clock.ticks == 1);
  REQUIRE(VideoTickNs(clock, clock.ticks) == 100001000);
  // Not due before the next tick.
  REQUIRE(!NextVideoStep(clock, 50001000, 2, 5).send);
  // No swap since: the tick repeats the frame.
  step = NextVideoStep(clock, 100001000, 1, 5);
  REQUIRE(step.send);
  REQUIRE(!step.capture);
  // Two ticks late: the missed one repeats, then a new capture.
  step = NextVideoStep(clock, 350001000, 2, 5);
  REQUIRE(step.repeats == 1);
  REQUIRE(step.capture);
  REQUIRE(step.dropped == 0);
  REQUIRE(clock.ticks == 4);
  // Further behind than max_late: those ticks drop.
  step = NextVideoStep(clock, 1300001000, 3, 5);
  REQUIRE(step.dropped == 9);
  REQUIRE(step.repeats == 0);
  REQUIRE(step.send);
  REQUIRE(clock.ticks == 14);
}

TEST_CASE("Video fits the guest output with black bars", "[automation]") {
  // 4x2 RGBX, stride 16, every pixel 100.
  std::vector<uint8_t> wide(16 * 2, 100);
  // Into 4x4: full width, a black row above and below.
  RgbImage tall = FitInto(wide.data(), 4, 2, 16, 4, 4);
  REQUIRE(tall.width == 4);
  REQUIRE(tall.height == 4);
  REQUIRE(tall.pixels[0] == 0);
  REQUIRE(tall.pixels[1 * 12] == 100);
  REQUIRE(tall.pixels[2 * 12 + 11] == 100);
  REQUIRE(tall.pixels[3 * 12] == 0);
  // Into 8x2: full height, black columns either side.
  RgbImage wider = FitInto(wide.data(), 4, 2, 16, 8, 2);
  REQUIRE(wider.pixels[0] == 0);
  REQUIRE(wider.pixels[2 * 3] == 100);
  REQUIRE(wider.pixels[5 * 3] == 100);
  REQUIRE(wider.pixels[6 * 3] == 0);
  // Scaled up, each output pixel takes its nearest source pixels.
  const uint8_t pair[] = {0, 0, 0, 0, 200, 200, 200, 0};
  RgbImage up = FitInto(pair, 2, 1, 8, 4, 2);
  REQUIRE(up.pixels[0] == 0);
  REQUIRE(up.pixels[1 * 3] == 0);
  REQUIRE(up.pixels[2 * 3] == 200);
  REQUIRE(up.pixels[4 * 3 + 9] == 200);
  // Black without an image.
  RgbImage black = FitInto(nullptr, 0, 0, 0, 4, 2);
  REQUIRE(black.pixels == std::vector<uint8_t>(4 * 2 * 3, 0));
}

TEST_CASE("Video replies and events describe the stream", "[automation]") {
  VideoOptions options;
  options.port = 5000;
  options.width = 640;
  options.height = 360;
  options.fps = 30;
  rapidjson::Document start = Json(VideoStartReply("4", options, 777));
  REQUIRE(start["re"] == 4);
  REQUIRE(start["pix_fmt"] == "rgb24");
  REQUIRE(start["width"] == 640);
  REQUIRE(start["fps"] == 30);
  REQUIRE(start["ns"] == 777);
  VideoCounts counts{90, 40, 2};
  rapidjson::Document stop = Json(VideoStopReply("5", counts));
  REQUIRE(stop["frames"] == 90);
  REQUIRE(stop["captures"] == 40);
  REQUIRE(stop["dropped"] == 2);
  EncodedFrame frame;
  frame.vblank = 70;
  frame.swap = 35;
  frame.ns = 123;
  frame.capture_ns = 5250000;
  rapidjson::Document event = Json(VideoFrameLine(12, frame));
  REQUIRE(event["type"] == "video_frame");
  REQUIRE(event["n"] == 12);
  REQUIRE(event["swap"] == 35);
  REQUIRE(event["capture_ms"] == 5.25);
  rapidjson::Document end =
      Json(telemetry::FinishNow(VideoEndEvent("gone", counts)) + "\n");
  REQUIRE(end["type"] == "video_end");
  REQUIRE(end["reason"] == "gone");
  REQUIRE(end["frames"] == 90);
}

TEST_CASE("Audio subscribes as a stream", "[automation]") {
  Session observer{Role::kObserver};
  rapidjson::Document reply = Json(
      Decide(observer,
             Parsed(R"({"op":"subscribe","streams":["telemetry","audio"]})"),
             TestContext())
          .reply);
  REQUIRE(reply["ok"] == true);
  REQUIRE(reply["streams"].Size() == 2);
  REQUIRE(reply["streams"][1] == "audio");
  REQUIRE(observer.audio);
  Decide(observer, Parsed(R"({"op":"subscribe","streams":[]})"), TestContext());
  REQUIRE(!observer.audio);
}

TEST_CASE("Guest audio frames downmix to stereo", "[automation]") {
  // Six planar channels of big-endian floats, the guest's layout.
  std::vector<float> frame(kGuestAudioChannels * kGuestAudioFrameSamples);
  auto put = [&frame](uint32_t channel, uint32_t i, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    bits = (bits >> 24) | ((bits >> 8) & 0xFF00) | ((bits << 8) & 0xFF0000) |
           (bits << 24);
    std::memcpy(&frame[channel * kGuestAudioFrameSamples + i], &bits,
                sizeof(bits));
  };
  // Sample 0: front left only. 1: centre, to both sides at -3 dB. 2: back
  // right and LFE (left out). 3: overdriven left, clamped. 4: NaN, silent.
  put(0, 0, 0.5f);
  put(2, 1, 0.5f);
  put(5, 2, 0.25f);
  put(3, 2, 1.0f);
  put(0, 3, 3.0f);
  put(1, 4, std::nanf(""));
  std::vector<int16_t> stereo(kGuestAudioFrameSamples * 2);
  DownmixGuestFrame(frame.data(), stereo.data());
  REQUIRE(stereo[0] == 16384);
  REQUIRE(stereo[1] == 0);
  REQUIRE(stereo[2] == 11585);
  REQUIRE(stereo[3] == 11585);
  REQUIRE(stereo[4] == 0);
  REQUIRE(stereo[5] == 5792);
  REQUIRE(stereo[6] == 32767);
  REQUIRE(stereo[9] == 0);
}

TEST_CASE("Audio gathers per client into contiguous chunks", "[automation]") {
  // 4-frame chunks, a gap over 10 ms starts a new one.
  AudioChunker chunker(4, 10000000);
  std::vector<AudioChunk> done;
  const int16_t two[] = {1, 2, 3, 4};
  chunker.Add(0, 1000, 7, 3, two, 2, done);
  chunker.Add(1, 1500, 7, 3, two, 2, done);
  REQUIRE(done.empty());
  chunker.Add(0, 2000, 8, 4, two, 2, done);
  REQUIRE(done.size() == 1);
  REQUIRE(done[0].client == 0);
  REQUIRE(done[0].ns == 1000);
  REQUIRE(done[0].vblank == 7);
  REQUIRE(done[0].frames() == 4);
  // Client 1's next frames come 50 ms later: its first chunk ends short.
  chunker.Add(1, 50001500, 9, 5, two, 2, done);
  REQUIRE(done.size() == 2);
  REQUIRE(done[1].client == 1);
  REQUIRE(done[1].frames() == 2);
  // Flushed once idle long enough.
  chunker.Flush(50001600, 200, done);
  REQUIRE(done.size() == 2);
  chunker.Flush(50001700, 200, done);
  REQUIRE(done.size() == 3);
  REQUIRE(done[2].ns == 50001500);
}

TEST_CASE("Audio events carry little-endian stereo", "[automation]") {
  AudioChunk chunk;
  chunk.client = 2;
  chunk.ns = 123;
  chunk.vblank = 70;
  chunk.swap = 35;
  chunk.samples = {1, -1};
  rapidjson::Document event =
      Json(AudioEventLine(chunk, telemetry::State::kRunning));
  REQUIRE(event["type"] == "audio");
  REQUIRE(event["ns"] == 123);
  REQUIRE(event["vblank"] == 70);
  REQUIRE(event["client"] == 2);
  REQUIRE(event["rate"] == 48000);
  REQUIRE(event["channels"] == 2);
  REQUIRE(event["frames"] == 1);
  // 01 00 FF FF
  REQUIRE(event["data"] == "AQD//w==");
}

}  // namespace test
}  // namespace automation
}  // namespace xe
