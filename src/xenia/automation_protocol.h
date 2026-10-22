/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_AUTOMATION_PROTOCOL_H_
#define XENIA_AUTOMATION_PROTOCOL_H_

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "third_party/rapidjson/include/rapidjson/document.h"
#include "third_party/rapidjson/include/rapidjson/stringbuffer.h"
#include "third_party/rapidjson/include/rapidjson/writer.h"
#include "xenia/answer_source.h"
#include "xenia/base/telemetry.h"
#include "xenia/hid/remote/remote_pad.h"

// The automation channel's protocol, kept free of sockets and threads: JSON
// lines in, decisions and JSON lines out. AutomationChannel carries them out.
// See docs/automation.md for the protocol.

namespace xe {
namespace automation {

constexpr uint32_t kProtocolVersion = 1;
// A longer line drops the client.
constexpr size_t kMaxLineLength = 16 * 1024 * 1024;
// The guest's vblank rate: no title swaps faster.
constexpr int64_t kMaxWatchFps = 60;
// One minute of vblanks at 60 Hz, as pad_queue's longest hold.
constexpr int64_t kMaxAdvanceVblanks = 3600;

// Splits a byte stream into lines, without the terminator (\n or \r\n).
class LineSplitter {
 public:
  void Append(std::string_view bytes) { buffer_.append(bytes); }

  std::optional<std::string> NextLine() {
    size_t end = buffer_.find('\n', scanned_);
    if (end == std::string::npos) {
      scanned_ = buffer_.size();
      return std::nullopt;
    }
    std::string line = buffer_.substr(0, end);
    buffer_.erase(0, end + 1);
    scanned_ = 0;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    return line;
  }

  // An unterminated line longer than kMaxLineLength.
  bool overflowed() const { return buffer_.size() > kMaxLineLength; }

 private:
  std::string buffer_;
  size_t scanned_ = 0;
};

struct Request {
  std::string op;
  // The client's id, as JSON text, echoed in the reply; empty without one.
  std::string id;
  // hello
  std::optional<uint32_t> protocol;
  std::string token;
  // subscribe
  std::optional<std::vector<std::string>> streams;
  // dialog_answer: the dialog's id, and the answer object as JSON text.
  std::optional<uint64_t> dialog;
  std::string answer;
  // pad_*: unchecked until ParsePadCommand.
  std::optional<int64_t> user;
  std::optional<bool> connected;
  std::optional<int64_t> input;
  std::optional<int64_t> frames;
  // The pad object as JSON text.
  std::string pad;
  // frame, frames_watch: unchecked until ParseFrameOptions.
  std::optional<std::string> format;
  std::optional<int64_t> max_width;
  std::optional<int64_t> quality;
  std::optional<int64_t> fps;
  // video_start: unchecked until ParseVideoOptions.
  std::optional<int64_t> port;
  std::optional<int64_t> width;
  std::optional<int64_t> height;
  // bench_stop
  std::optional<bool> raw;
  // advance
  std::optional<int64_t> vblanks;
};

struct ParseResult {
  std::optional<Request> request;
  std::string error;
  // The id, if one could be read, for the error reply.
  std::string id;
};

inline std::string ToJson(const rapidjson::Value& value) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  value.Accept(writer);
  return std::string(buffer.GetString(), buffer.GetSize());
}

inline ParseResult ParseRequest(std::string_view line) {
  ParseResult result;
  rapidjson::Document document;
  document.Parse(line.data(), line.size());
  if (document.HasParseError() || !document.IsObject()) {
    result.error = "not a JSON object";
    return result;
  }
  auto id = document.FindMember("id");
  if (id != document.MemberEnd()) {
    if (!id->value.IsString() && !id->value.IsNumber()) {
      result.error = "id must be a string or a number";
      return result;
    }
    result.id = ToJson(id->value);
  }
  auto op = document.FindMember("op");
  if (op == document.MemberEnd() || !op->value.IsString()) {
    result.error = "missing op";
    return result;
  }
  Request request;
  request.op = op->value.GetString();
  request.id = result.id;
  auto protocol = document.FindMember("protocol");
  if (protocol != document.MemberEnd()) {
    if (!protocol->value.IsUint()) {
      result.error = "protocol must be a number";
      return result;
    }
    request.protocol = protocol->value.GetUint();
  }
  auto token = document.FindMember("token");
  if (token != document.MemberEnd()) {
    if (!token->value.IsString()) {
      result.error = "token must be a string";
      return result;
    }
    request.token = token->value.GetString();
  }
  auto streams = document.FindMember("streams");
  if (streams != document.MemberEnd()) {
    if (!streams->value.IsArray()) {
      result.error = "streams must be a list";
      return result;
    }
    request.streams.emplace();
    for (const auto& stream : streams->value.GetArray()) {
      if (!stream.IsString()) {
        result.error = "streams must be a list of names";
        return result;
      }
      request.streams->emplace_back(stream.GetString());
    }
  }
  auto dialog = document.FindMember("dialog");
  if (dialog != document.MemberEnd()) {
    if (!dialog->value.IsUint64()) {
      result.error = "dialog must be a number";
      return result;
    }
    request.dialog = dialog->value.GetUint64();
  }
  auto answer = document.FindMember("answer");
  if (answer != document.MemberEnd()) {
    if (!answer->value.IsObject()) {
      result.error = "answer must be an object";
      return result;
    }
    request.answer = ToJson(answer->value);
  }
  for (auto [key, field] :
       {std::pair{"user", &request.user}, std::pair{"input", &request.input},
        std::pair{"frames", &request.frames},
        std::pair{"max_width", &request.max_width},
        std::pair{"quality", &request.quality}, std::pair{"fps", &request.fps},
        std::pair{"port", &request.port}, std::pair{"width", &request.width},
        std::pair{"height", &request.height},
        std::pair{"vblanks", &request.vblanks}}) {
    auto member = document.FindMember(key);
    if (member != document.MemberEnd()) {
      if (!member->value.IsInt64()) {
        result.error = std::string(key) + " must be a number";
        return result;
      }
      *field = member->value.GetInt64();
    }
  }
  auto connected = document.FindMember("connected");
  if (connected != document.MemberEnd()) {
    if (!connected->value.IsBool()) {
      result.error = "connected must be true or false";
      return result;
    }
    request.connected = connected->value.GetBool();
  }
  auto pad = document.FindMember("pad");
  if (pad != document.MemberEnd()) {
    if (!pad->value.IsObject()) {
      result.error = "pad must be an object";
      return result;
    }
    request.pad = ToJson(pad->value);
  }
  auto format = document.FindMember("format");
  if (format != document.MemberEnd()) {
    if (!format->value.IsString()) {
      result.error = "format must be a string";
      return result;
    }
    request.format = format->value.GetString();
  }
  auto raw = document.FindMember("raw");
  if (raw != document.MemberEnd()) {
    if (!raw->value.IsBool()) {
      result.error = "raw must be true or false";
      return result;
    }
    request.raw = raw->value.GetBool();
  }
  result.request = std::move(request);
  return result;
}

// Compares in time independent of where the strings differ.
inline bool TokensEqual(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  unsigned char diff = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  }
  return diff == 0;
}

// A token from random bytes, as lowercase hex.
inline std::string TokenFromBytes(const std::array<uint8_t, 16>& bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string token;
  for (uint8_t byte : bytes) {
    token.push_back(kHex[byte >> 4]);
    token.push_back(kHex[byte & 0xF]);
  }
  return token;
}

enum class Role { kNone, kController, kObserver };

inline const char* RoleName(Role role) {
  switch (role) {
    case Role::kController:
      return "controller";
    case Role::kObserver:
      return "observer";
    default:
      return "none";
  }
}

struct BuildInfo {
  std::string branch;
  std::string commit;
  std::string date;
};

// What the emulator is doing, as the channel last heard.
struct Snapshot {
  bool title_open = false;
  uint32_t title_id = 0;
  std::string title_name;
};

struct Context {
  std::string_view token;
  // No other client has the controller role.
  bool controller_free = true;
  BuildInfo build;
  Snapshot snapshot;
};

enum class FrameFormat { kJpeg, kPng };

// How a frame is encoded. A max_width of 0 keeps the guest output's width.
struct FrameOptions {
  FrameFormat format = FrameFormat::kJpeg;
  uint32_t max_width = 0;
  uint32_t quality = 85;
  bool operator==(const FrameOptions&) const = default;
};

// A frames_watch: frames at up to fps a second, each of a new swap.
struct FrameWatch {
  FrameOptions options;
  uint32_t fps = 0;
  // When the next frame is due, in host ns; 0 for now.
  uint64_t next_ns = 0;
  // The swap of the last frame sent, none before the first.
  std::optional<uint64_t> last_swap;
};

// A video_start: fps frames a second of width x height rgb24, sent to the
// listener on a loopback port.
struct VideoOptions {
  uint16_t port = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t fps = 0;
};

// What a video stream has sent: every frame, the new captures among them,
// the rest repeats, and the ticks dropped when it fell too far behind.
struct VideoCounts {
  uint64_t frames = 0;
  uint64_t captures = 0;
  uint64_t dropped = 0;
};

// A bench's samples, gathered from the frame_stats lines written since
// bench_start.
struct BenchSamples {
  std::vector<double> swap_ms;
  std::vector<uint64_t> swap_compiles;
  std::vector<double> present_ms;
  std::vector<double> present_latency_ms;
  uint64_t windows = 0;
  // The envelope ns of the first and last frame_stats, and the vblanks of the
  // windows after the first: those that ended between the two.
  uint64_t first_ns = 0;
  uint64_t last_ns = 0;
  uint64_t vblanks_after_first = 0;
};

// One connection's state.
struct Session {
  Role role = Role::kNone;
  // Subscribed to telemetry events.
  bool telemetry = false;
  // Subscribed to dialog events; the controller answers them.
  bool dialogs = false;
  // Subscribed to pad events.
  bool input = false;
  // Subscribed to audio events.
  bool audio = false;
  // Watching frames.
  std::optional<FrameWatch> frames;
  // Benching, since bench_start.
  std::optional<BenchSamples> bench;
};

// Telemetry's first line, describing the build, host and settings; a new
// telemetry subscriber gets it first, however late it subscribes.
inline bool IsSessionStart(std::string_view telemetry_line) {
  return telemetry_line.starts_with(R"({"type":"session_start")");
}

enum class Action {
  kNone,
  // Send the reply, then close this connection.
  kClose,
  // Send the reply and the exit event, then end the process.
  kStop,
  // Look up the dialog and reply with DecideDialogAnswer.
  kAnswerDialog,
  // Apply pad, and reply with PadReply.
  kPad,
  // Capture a frame with frame_options, and reply with FrameReply.
  kFrame,
  // Connect to the listener in video_options, and reply with VideoStartReply.
  kVideoStart,
  // End this client's video stream, and reply with VideoStopReply.
  kVideoStop,
  // Pause or resume the emulator, and reply with PauseReply.
  kPause,
  kResume,
  // Run the paused emulator for vblanks, pause it again, then reply with
  // PauseReply.
  kAdvance,
  // Snapshot the guest threads, and reply with ThreadsReply.
  kThreads,
};

struct Decision {
  std::string reply;
  Action action = Action::kNone;
  std::optional<hid::remote::Command> pad;
  FrameOptions frame_options;
  VideoOptions video_options;
  uint64_t vblanks = 0;
};

class JsonLine {
 public:
  JsonLine() { writer_.StartObject(); }

  JsonLine& Raw(const char* key, std::string_view json) {
    writer_.Key(key);
    writer_.RawValue(json.data(), json.size(), rapidjson::kObjectType);
    return *this;
  }
  JsonLine& String(const char* key, std::string_view value) {
    writer_.Key(key);
    writer_.String(value.data(), rapidjson::SizeType(value.size()));
    return *this;
  }
  JsonLine& Uint(const char* key, uint64_t value) {
    writer_.Key(key);
    writer_.Uint64(value);
    return *this;
  }
  JsonLine& Int(const char* key, int64_t value) {
    writer_.Key(key);
    writer_.Int64(value);
    return *this;
  }
  JsonLine& Bool(const char* key, bool value) {
    writer_.Key(key);
    writer_.Bool(value);
    return *this;
  }
  JsonLine& Double(const char* key, double value) {
    writer_.Key(key);
    writer_.Double(value);
    return *this;
  }
  // The reply's correlation: re = the request's id, when it had one.
  JsonLine& Re(std::string_view id) {
    return id.empty() ? *this : Raw("re", id);
  }

  std::string Finish() {
    writer_.EndObject();
    return std::string(buffer_.GetString(), buffer_.GetSize()) + "\n";
  }

 private:
  rapidjson::StringBuffer buffer_;
  rapidjson::Writer<rapidjson::StringBuffer> writer_{buffer_};
};

inline std::string ErrorLine(std::string_view id, std::string_view error) {
  return JsonLine().Re(id).Bool("ok", false).String("error", error).Finish();
}

// Title ids are written as 8 hex digits, as everywhere else.
inline std::string TitleIdText(uint32_t title_id) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string text(8, '0');
  for (int i = 7; i >= 0; --i, title_id >>= 4) {
    text[i] = kHex[title_id & 0xF];
  }
  return text;
}

inline std::string SnapshotJson(const Snapshot& snapshot) {
  JsonLine line;
  line.Bool("title_open", snapshot.title_open);
  if (snapshot.title_open) {
    line.String("title_id", TitleIdText(snapshot.title_id))
        .String("title", snapshot.title_name);
  }
  std::string json = line.Finish();
  json.pop_back();
  return json;
}

// Defined with the dialogs and the pads below.
inline std::string StringListJson(const std::vector<std::string>& values);
inline std::optional<hid::remote::Command> ParsePadCommand(
    const Request& request, std::string* error);
inline std::optional<FrameOptions> ParseFrameOptions(const Request& request,
                                                     std::string* error);
inline std::optional<VideoOptions> ParseVideoOptions(const Request& request,
                                                     std::string* error);
inline std::string BenchReply(std::string_view id, const BenchSamples& samples,
                              bool raw);

// What to do with one request on a connection.
inline Decision Decide(Session& session, const Request& request,
                       const Context& context) {
  if (session.role == Role::kNone) {
    if (request.op != "hello") {
      return {ErrorLine(request.id, "hello first"), Action::kClose};
    }
    if (request.protocol != kProtocolVersion) {
      return {JsonLine()
                  .Re(request.id)
                  .Bool("ok", false)
                  .String("error", "unsupported protocol")
                  .Uint("protocol", kProtocolVersion)
                  .Finish(),
              Action::kClose};
    }
    if (!TokensEqual(request.token, context.token)) {
      return {ErrorLine(request.id, "bad token"), Action::kClose};
    }
    session.role =
        context.controller_free ? Role::kController : Role::kObserver;
    std::string xenia = JsonLine()
                            .String("branch", context.build.branch)
                            .String("commit", context.build.commit)
                            .String("date", context.build.date)
                            .Finish();
    xenia.pop_back();
    return {JsonLine()
                .Re(request.id)
                .Bool("ok", true)
                .Uint("protocol", kProtocolVersion)
                .Raw("xenia", xenia)
                .Raw("capabilities",
                     R"(["state","stop","subscribe","dialog_answer",)"
                     R"("pad_connect","pad_set","pad_queue","pad_clear",)"
                     R"("frame","frames_watch","bench_start","bench_stop",)"
                     R"("video_start","video_stop","pause","resume",)"
                     R"("advance","threads"])")
                .Raw("streams", R"(["telemetry","dialogs","input","audio"])")
                .String("role", RoleName(session.role))
                .Raw("state", SnapshotJson(context.snapshot))
                .Finish()};
  }
  if (request.op == "hello") {
    return {ErrorLine(request.id, "already said hello")};
  }
  if (request.op == "state") {
    return {JsonLine()
                .Re(request.id)
                .Bool("ok", true)
                .Raw("state", SnapshotJson(context.snapshot))
                .Finish()};
  }
  if (request.op == "subscribe") {
    if (!request.streams) {
      return {ErrorLine(request.id, "missing streams")};
    }
    Session subscribed = session;
    subscribed.telemetry = subscribed.dialogs = subscribed.input =
        subscribed.audio = false;
    for (const std::string& stream : *request.streams) {
      if (stream == "telemetry") {
        subscribed.telemetry = true;
      } else if (stream == "dialogs") {
        subscribed.dialogs = true;
      } else if (stream == "input") {
        subscribed.input = true;
      } else if (stream == "audio") {
        subscribed.audio = true;
      } else {
        return {ErrorLine(request.id, "unknown stream " + stream)};
      }
    }
    session = subscribed;
    std::vector<std::string> streams;
    if (session.telemetry) {
      streams.push_back("telemetry");
    }
    if (session.dialogs) {
      streams.push_back("dialogs");
    }
    if (session.input) {
      streams.push_back("input");
    }
    if (session.audio) {
      streams.push_back("audio");
    }
    return {JsonLine()
                .Re(request.id)
                .Bool("ok", true)
                .Raw("streams", StringListJson(streams))
                .Finish()};
  }
  if (request.op.starts_with("pad_")) {
    if (session.role != Role::kController) {
      return {ErrorLine(request.id, "needs the controller role")};
    }
    std::string error;
    std::optional<hid::remote::Command> command =
        ParsePadCommand(request, &error);
    if (!command) {
      return {ErrorLine(request.id, error)};
    }
    return {"", Action::kPad, command};
  }
  if (request.op == "bench_start") {
    session.bench.emplace();
    return {JsonLine().Re(request.id).Bool("ok", true).Finish()};
  }
  if (request.op == "bench_stop") {
    if (!session.bench) {
      return {ErrorLine(request.id, "no bench running")};
    }
    std::string reply =
        BenchReply(request.id, *session.bench, request.raw.value_or(false));
    session.bench.reset();
    return {std::move(reply)};
  }
  if (request.op == "frame") {
    std::string error;
    std::optional<FrameOptions> options = ParseFrameOptions(request, &error);
    if (!options) {
      return {ErrorLine(request.id, error)};
    }
    Decision decision{"", Action::kFrame};
    decision.frame_options = *options;
    return decision;
  }
  if (request.op == "frames_watch") {
    if (!request.fps || *request.fps < 0 || *request.fps > kMaxWatchFps) {
      return {ErrorLine(
          request.id, "fps must be from 0 to " + std::to_string(kMaxWatchFps))};
    }
    std::string error;
    std::optional<FrameOptions> options = ParseFrameOptions(request, &error);
    if (!options) {
      return {ErrorLine(request.id, error)};
    }
    if (*request.fps == 0) {
      session.frames.reset();
    } else {
      FrameWatch watch;
      watch.options = *options;
      watch.fps = uint32_t(*request.fps);
      session.frames = watch;
    }
    return {JsonLine().Re(request.id).Bool("ok", true).Finish()};
  }
  if (request.op == "video_start") {
    std::string error;
    std::optional<VideoOptions> options = ParseVideoOptions(request, &error);
    if (!options) {
      return {ErrorLine(request.id, error)};
    }
    Decision decision{"", Action::kVideoStart};
    decision.video_options = *options;
    return decision;
  }
  if (request.op == "video_stop") {
    return {"", Action::kVideoStop};
  }
  if (request.op == "dialog_answer") {
    if (session.role != Role::kController) {
      return {ErrorLine(request.id, "needs the controller role")};
    }
    if (!request.dialog || request.answer.empty()) {
      return {ErrorLine(request.id, "missing dialog or answer")};
    }
    return {"", Action::kAnswerDialog};
  }
  if (request.op == "stop") {
    if (session.role != Role::kController) {
      return {ErrorLine(request.id, "needs the controller role")};
    }
    return {JsonLine().Re(request.id).Bool("ok", true).Finish(), Action::kStop};
  }
  if (request.op == "threads") {
    return {"", Action::kThreads};
  }
  if (request.op == "pause" || request.op == "resume" ||
      request.op == "advance") {
    if (session.role != Role::kController) {
      return {ErrorLine(request.id, "needs the controller role")};
    }
    if (request.op == "pause") {
      return {"", Action::kPause};
    }
    if (request.op == "resume") {
      return {"", Action::kResume};
    }
    if (!request.vblanks || *request.vblanks < 1 ||
        *request.vblanks > kMaxAdvanceVblanks) {
      return {ErrorLine(request.id, "vblanks must be from 1 to " +
                                        std::to_string(kMaxAdvanceVblanks))};
    }
    Decision decision{"", Action::kAdvance};
    decision.vblanks = uint64_t(*request.vblanks);
    return decision;
  }
  return {ErrorLine(request.id, "unknown op")};
}

// Dialogs. Each open dialog is announced with a dialog event carrying the
// request and the policy's answer as its default; the controller may answer it
// before its timeout, then dialog_closed says who answered.

// Controller slots, as XUserMaxUserCount.
constexpr uint32_t kMaxUsers = 4;

inline const char* DialogKind(const DialogRequest& request) {
  static constexpr const char* kKinds[] = {"choice", "text", "sign_in",
                                           "passcode", "disc"};
  return kKinds[request.index()];
}

// XUIDs travel as 16 hex digits: JSON numbers lose 64-bit precision in many
// clients.
inline std::string XuidText(uint64_t xuid) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string text(16, '0');
  for (int i = 15; i >= 0; --i, xuid >>= 4) {
    text[i] = kHex[xuid & 0xF];
  }
  return text;
}

inline std::optional<uint64_t> ParseXuid(std::string_view text) {
  if (text.empty() || text.size() > 16) {
    return std::nullopt;
  }
  uint64_t xuid = 0;
  for (char c : text) {
    uint64_t digit;
    if (c >= '0' && c <= '9') {
      digit = uint64_t(c) - '0';
    } else if (c >= 'A' && c <= 'F') {
      digit = uint64_t(c) - 'A' + 10;
    } else if (c >= 'a' && c <= 'f') {
      digit = uint64_t(c) - 'a' + 10;
    } else {
      return std::nullopt;
    }
    xuid = (xuid << 4) | digit;
  }
  return xuid;
}

inline std::string PathText(const std::filesystem::path& path) {
  std::u8string text = path.u8string();
  return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

inline std::string StringListJson(const std::vector<std::string>& values) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writer.StartArray();
  for (const std::string& value : values) {
    writer.String(value.data(), rapidjson::SizeType(value.size()));
  }
  writer.EndArray();
  return std::string(buffer.GetString(), buffer.GetSize());
}

inline std::string ProfilesJson(const std::map<uint8_t, uint64_t>& profiles) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writer.StartArray();
  for (const auto& [slot, xuid] : profiles) {
    writer.StartObject();
    writer.Key("slot");
    writer.Uint(slot);
    writer.Key("xuid");
    std::string text = XuidText(xuid);
    writer.String(text.data(), rapidjson::SizeType(text.size()));
    writer.EndObject();
  }
  writer.EndArray();
  return std::string(buffer.GetString(), buffer.GetSize());
}

// An answer as the protocol writes it: the dialog event's default, and what a
// client sends back.
inline std::string AnswerJson(const DialogAnswer& answer) {
  JsonLine line;
  switch (answer.index()) {
    case 0:
      line.Uint("button", std::get<0>(answer));
      break;
    case 1:
      if (const auto& text = std::get<1>(answer)) {
        line.String("text", *text);
      } else {
        line.Bool("cancel", true);
      }
      break;
    case 2:
      if (const auto& profiles = std::get<2>(answer)) {
        line.Raw("profiles", ProfilesJson(*profiles));
      } else {
        line.Bool("cancel", true);
      }
      break;
    case 3:
      if (const auto& keys = std::get<3>(answer)) {
        std::string list = "[";
        for (size_t i = 0; i < keys->size(); ++i) {
          list += (i ? "," : "") + std::to_string((*keys)[i]);
        }
        line.Raw("keys", list + "]");
      } else {
        line.Bool("cancel", true);
      }
      break;
    default:
      if (std::get<4>(answer).empty()) {
        line.Bool("cancel", true);
      } else {
        line.String("path", PathText(std::get<4>(answer)));
      }
      break;
  }
  std::string json = line.Finish();
  json.pop_back();
  return json;
}

inline telemetry::Event DialogEvent(uint64_t id, const DialogRequest& request,
                                    const DialogAnswer& default_answer,
                                    uint32_t timeout_ms) {
  telemetry::Event event("dialog");
  event.UInt("dialog", id).Str("kind", DialogKind(request));
  switch (request.index()) {
    case 0: {
      const ChoiceRequest& choice = std::get<0>(request);
      event.Str("title", choice.title)
          .Str("text", choice.text)
          .Raw("buttons", StringListJson(choice.buttons));
      break;
    }
    case 1: {
      const TextRequest& text = std::get<1>(request);
      event.Str("title", text.title)
          .Str("description", text.description)
          .Str("default_text", text.default_text)
          .UInt("max_length", text.max_length);
      break;
    }
    case 2: {
      const SignInRequest& sign_in = std::get<2>(request);
      event.UInt("users_needed", sign_in.users_needed)
          .UInt("last_used_slot", sign_in.last_used_slot)
          .Raw("signed_in", ProfilesJson(sign_in.signed_in));
      break;
    }
    case 3: {
      const PasscodeRequest& passcode = std::get<3>(request);
      event.Str("title", passcode.title)
          .Str("description", passcode.description)
          .UInt("length", passcode.length);
      break;
    }
    default:
      event.Str("prompt", std::get<4>(request).prompt);
      break;
  }
  event.Raw("default", AnswerJson(default_answer))
      .UInt("timeout_ms", timeout_ms);
  return event;
}

// by: "client", "timeout" or "policy" (no controller to ask any more).
inline telemetry::Event DialogClosedEvent(uint64_t id, std::string_view by) {
  telemetry::Event event("dialog_closed");
  event.UInt("dialog", id).Str("by", by);
  return event;
}

// Checks a client's answer against the open request: what a real dialog
// would let a person pick.
inline std::optional<DialogAnswer> ParseDialogAnswer(
    const DialogRequest& request, std::string_view json, std::string* error) {
  rapidjson::Document answer;
  answer.Parse(json.data(), json.size());
  if (answer.HasParseError() || !answer.IsObject()) {
    *error = "answer must be an object";
    return std::nullopt;
  }
  auto cancel = answer.FindMember("cancel");
  if (cancel != answer.MemberEnd() && cancel->value.IsBool() &&
      cancel->value.GetBool()) {
    switch (request.index()) {
      case 0:
        *error = "a choice can't be cancelled";
        return std::nullopt;
      case 1:
        return DialogAnswer(std::in_place_index<1>);
      case 2:
        return DialogAnswer(std::in_place_index<2>);
      case 3:
        return DialogAnswer(std::in_place_index<3>);
      default:
        return DialogAnswer(std::in_place_index<4>);
    }
  }
  switch (request.index()) {
    case 0: {
      auto button = answer.FindMember("button");
      size_t count = std::get<0>(request).buttons.size();
      if (button == answer.MemberEnd() || !button->value.IsUint() ||
          button->value.GetUint() >= std::max<size_t>(count, 1)) {
        *error = "button must be one of the buttons' indexes";
        return std::nullopt;
      }
      return DialogAnswer(std::in_place_index<0>, button->value.GetUint());
    }
    case 1: {
      auto text = answer.FindMember("text");
      if (text == answer.MemberEnd() || !text->value.IsString()) {
        *error = "text must be a string, or cancel true";
        return std::nullopt;
      }
      size_t max_length = std::get<1>(request).max_length;
      if (max_length && text->value.GetStringLength() > max_length) {
        *error = "text is longer than max_length";
        return std::nullopt;
      }
      return DialogAnswer(
          std::in_place_index<1>,
          std::string(text->value.GetString(), text->value.GetStringLength()));
    }
    case 2: {
      auto profiles = answer.FindMember("profiles");
      if (profiles == answer.MemberEnd() || !profiles->value.IsArray()) {
        *error = "profiles must be a list, or cancel true";
        return std::nullopt;
      }
      std::map<uint8_t, uint64_t> chosen;
      for (const auto& profile : profiles->value.GetArray()) {
        auto slot = profile.IsObject() ? profile.FindMember("slot")
                                       : profile.MemberEnd();
        auto xuid = profile.IsObject() ? profile.FindMember("xuid")
                                       : profile.MemberEnd();
        std::optional<uint64_t> parsed_xuid;
        if (xuid != profile.MemberEnd() && xuid->value.IsString()) {
          parsed_xuid = ParseXuid(xuid->value.GetString());
        }
        if (slot == profile.MemberEnd() || !slot->value.IsUint() ||
            slot->value.GetUint() >= kMaxUsers || !parsed_xuid) {
          *error = "each profile needs a slot below 4 and a hex xuid";
          return std::nullopt;
        }
        if (!chosen.emplace(uint8_t(slot->value.GetUint()), *parsed_xuid)
                 .second) {
          *error = "a slot is given twice";
          return std::nullopt;
        }
      }
      if (chosen.size() > std::get<2>(request).users_needed) {
        *error = "more profiles than users_needed";
        return std::nullopt;
      }
      return DialogAnswer(std::in_place_index<2>, std::move(chosen));
    }
    case 3: {
      auto keys = answer.FindMember("keys");
      if (keys == answer.MemberEnd() || !keys->value.IsArray() ||
          keys->value.Size() != std::get<3>(request).length) {
        *error = "keys must be a list of length keys, or cancel true";
        return std::nullopt;
      }
      std::vector<uint16_t> values;
      for (const auto& key : keys->value.GetArray()) {
        if (!key.IsUint() || key.GetUint() > 0xFFFF) {
          *error = "each key must be a number up to 65535";
          return std::nullopt;
        }
        values.push_back(uint16_t(key.GetUint()));
      }
      return DialogAnswer(std::in_place_index<3>, std::move(values));
    }
    default: {
      auto path = answer.FindMember("path");
      if (path == answer.MemberEnd() || !path->value.IsString() ||
          !path->value.GetStringLength()) {
        *error = "path must be a host path, or cancel true";
        return std::nullopt;
      }
      std::u8string text(
          reinterpret_cast<const char8_t*>(path->value.GetString()),
          path->value.GetStringLength());
      return DialogAnswer(std::in_place_index<4>, std::filesystem::path(text));
    }
  }
}

struct DialogAnswerDecision {
  std::string reply;
  // Present when the dialog should close with it.
  std::optional<DialogAnswer> answer;
};

// pending: the open dialog the request names, or null if none is open.
inline DialogAnswerDecision DecideDialogAnswer(const Request& request,
                                               const DialogRequest* pending) {
  if (!pending) {
    return {ErrorLine(request.id, "no such open dialog")};
  }
  std::string error;
  std::optional<DialogAnswer> answer =
      ParseDialogAnswer(*pending, request.answer, &error);
  if (!answer) {
    return {ErrorLine(request.id, error)};
  }
  return {JsonLine().Re(request.id).Bool("ok", true).Finish(),
          std::move(answer)};
}

// Controller input. pad_connect, pad_set, pad_queue and pad_clear drive the
// four controller slots with exact values and vblank timing; see
// hid/remote/remote_pad.h.

inline std::optional<hid::remote::PadState> ParsePad(std::string_view json,
                                                     std::string* error) {
  rapidjson::Document pad;
  pad.Parse(json.data(), json.size());
  if (pad.HasParseError() || !pad.IsObject()) {
    *error = "pad must be an object";
    return std::nullopt;
  }
  hid::remote::PadState state;
  struct Field {
    const char* key;
    int64_t min;
    int64_t max;
    int64_t* value;
  };
  int64_t buttons = 0, lt = 0, rt = 0, lx = 0, ly = 0, rx = 0, ry = 0;
  for (const Field& field :
       {Field{"buttons", 0, 0xFFFF, &buttons}, Field{"lt", 0, 255, &lt},
        Field{"rt", 0, 255, &rt}, Field{"lx", -32768, 32767, &lx},
        Field{"ly", -32768, 32767, &ly}, Field{"rx", -32768, 32767, &rx},
        Field{"ry", -32768, 32767, &ry}}) {
    auto member = pad.FindMember(field.key);
    if (member == pad.MemberEnd()) {
      continue;
    }
    if (!member->value.IsInt64() || member->value.GetInt64() < field.min ||
        member->value.GetInt64() > field.max) {
      *error = std::string("pad ") + field.key + " must be from " +
               std::to_string(field.min) + " to " + std::to_string(field.max);
      return std::nullopt;
    }
    *field.value = member->value.GetInt64();
  }
  state.buttons = uint16_t(buttons);
  state.left_trigger = uint8_t(lt);
  state.right_trigger = uint8_t(rt);
  state.thumb_lx = int16_t(lx);
  state.thumb_ly = int16_t(ly);
  state.thumb_rx = int16_t(rx);
  state.thumb_ry = int16_t(ry);
  return state;
}

// A pad_* request as a checked command for RemotePads.
inline std::optional<hid::remote::Command> ParsePadCommand(
    const Request& request, std::string* error) {
  using hid::remote::Command;
  if (!request.user || *request.user < 0 ||
      *request.user >= int64_t(hid::remote::kUserCount)) {
    *error = "user must be from 0 to 3";
    return std::nullopt;
  }
  Command command;
  command.user = uint32_t(*request.user);
  if (request.op == "pad_connect") {
    if (!request.connected) {
      *error = "missing connected";
      return std::nullopt;
    }
    command.type = Command::Type::kConnect;
    command.value = *request.connected ? 1 : 0;
    return command;
  }
  if (request.op == "pad_clear") {
    command.type = Command::Type::kClear;
    return command;
  }
  if (request.op != "pad_set" && request.op != "pad_queue") {
    *error = "unknown op";
    return std::nullopt;
  }
  if (request.pad.empty()) {
    *error = "missing pad";
    return std::nullopt;
  }
  std::optional<hid::remote::PadState> state = ParsePad(request.pad, error);
  if (!state) {
    return std::nullopt;
  }
  command.state = *state;
  if (request.op == "pad_set") {
    command.type = Command::Type::kSet;
    return command;
  }
  if (!request.input || *request.input < 1 || *request.input > INT32_MAX) {
    *error = "input must be a positive number";
    return std::nullopt;
  }
  if (!request.frames || *request.frames < 1 ||
      *request.frames > int64_t(hid::remote::kMaxFrames)) {
    *error =
        "frames must be from 1 to " + std::to_string(hid::remote::kMaxFrames);
    return std::nullopt;
  }
  command.type = Command::Type::kQueue;
  command.id = uint32_t(*request.input);
  command.frames = uint32_t(*request.frames);
  return command;
}

// error: what RemotePads said, empty on success.
// Whether the emulator is now paused, and the guest's vblank and swap counts.
inline std::string PauseReply(std::string_view id, bool paused, uint64_t vblank,
                              uint64_t swap) {
  return JsonLine()
      .Re(id)
      .Bool("ok", true)
      .Bool("paused", paused)
      .Uint("vblank", vblank)
      .Uint("swap", swap)
      .Finish();
}

inline std::string PadReply(std::string_view id, std::string_view error) {
  return error.empty() ? JsonLine().Re(id).Bool("ok", true).Finish()
                       : ErrorLine(id, error);
}

// pad_begin: a queued state became visible, from at_vblank; pad_seen: the
// guest first read it; pad_end: it stopped being visible.
inline telemetry::Event PadEvent(const hid::remote::PadEvent& pad_event) {
  static constexpr const char* kTypes[] = {"pad_begin", "pad_seen", "pad_end"};
  telemetry::Event event(kTypes[size_t(pad_event.kind)]);
  event.UInt("user", pad_event.user)
      .UInt("input", pad_event.id)
      .UInt("at_vblank", pad_event.vblank);
  return event;
}

// Frames. frame captures the guest output once; frames_watch sends frame events
// at up to fps a second. Both scale to max_width and encode as JPEG or PNG.

inline std::optional<FrameOptions> ParseFrameOptions(const Request& request,
                                                     std::string* error) {
  FrameOptions options;
  if (request.format) {
    if (*request.format == "jpeg") {
      options.format = FrameFormat::kJpeg;
    } else if (*request.format == "png") {
      options.format = FrameFormat::kPng;
    } else {
      *error = "format must be jpeg or png";
      return std::nullopt;
    }
  }
  if (request.max_width) {
    if (*request.max_width < 0 || *request.max_width > 8192) {
      *error = "max_width must be from 0 to 8192";
      return std::nullopt;
    }
    options.max_width = uint32_t(*request.max_width);
  }
  if (request.quality) {
    if (*request.quality < 1 || *request.quality > 100) {
      *error = "quality must be from 1 to 100";
      return std::nullopt;
    }
    options.quality = uint32_t(*request.quality);
  }
  return options;
}

inline const char* FrameFormatName(FrameFormat format) {
  return format == FrameFormat::kPng ? "png" : "jpeg";
}

// A watch wants a frame now: its interval has passed and the guest has
// swapped since its last one.
inline bool FrameDue(const FrameWatch& watch, uint64_t now_ns, uint64_t swap) {
  return now_ns >= watch.next_ns && watch.last_swap != swap;
}

// After a frame of swap was taken for watch at now_ns. The next is due one
// interval after this one was, so the rate holds; after a stall longer than an
// interval, one interval from now, so no burst follows.
inline void AdvanceWatch(FrameWatch& watch, uint64_t now_ns, uint64_t swap) {
  uint64_t interval = 1000000000 / std::max(watch.fps, uint32_t(1));
  uint64_t next = watch.next_ns + interval;
  watch.next_ns = next > now_ns ? next : now_ns + interval;
  watch.last_swap = swap;
}

inline std::string Base64(const uint8_t* data, size_t size) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string text;
  text.reserve((size + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 3 <= size; i += 3) {
    uint32_t bits = uint32_t(data[i]) << 16 | uint32_t(data[i + 1]) << 8 |
                    uint32_t(data[i + 2]);
    text.push_back(kAlphabet[bits >> 18]);
    text.push_back(kAlphabet[(bits >> 12) & 63]);
    text.push_back(kAlphabet[(bits >> 6) & 63]);
    text.push_back(kAlphabet[bits & 63]);
  }
  if (i < size) {
    uint32_t bits = uint32_t(data[i]) << 16;
    if (i + 1 < size) {
      bits |= uint32_t(data[i + 1]) << 8;
    }
    text.push_back(kAlphabet[bits >> 18]);
    text.push_back(kAlphabet[(bits >> 12) & 63]);
    text.push_back(i + 1 < size ? kAlphabet[(bits >> 6) & 63] : '=');
    text.push_back('=');
  }
  return text;
}

struct RgbImage {
  uint32_t width = 0;
  uint32_t height = 0;
  // R8 G8 B8, rows packed.
  std::vector<uint8_t> pixels;
};

// R8 G8 B8 X8 rows (as ui::RawImage) scaled to out_width x out_height and
// written as R8 G8 B8 to out, out_stride bytes a row. Each output pixel is the
// average of the source pixels it touches.
inline void ScaleInto(const uint8_t* rgbx, uint32_t width, uint32_t height,
                      size_t stride, uint8_t* out, size_t out_stride,
                      uint32_t out_width, uint32_t out_height) {
  if (!width || !height || !out_width || !out_height) {
    return;
  }
  if (out_width == width && out_height == height) {
    for (uint32_t y = 0; y < height; ++y) {
      const uint8_t* row = rgbx + size_t(y) * stride;
      uint8_t* pixel = out + size_t(y) * out_stride;
      for (uint32_t x = 0; x < width; ++x, row += 4, pixel += 3) {
        pixel[0] = row[0];
        pixel[1] = row[1];
        pixel[2] = row[2];
      }
    }
    return;
  }
  // Each output pixel touches at least one source pixel, one or two when
  // scaling up. The source columns of each output column, computed once.
  std::vector<uint32_t> x0s(out_width), x1s(out_width);
  for (uint32_t x = 0; x < out_width; ++x) {
    x0s[x] = uint32_t(uint64_t(x) * width / out_width);
    x1s[x] = uint32_t((uint64_t(x + 1) * width + out_width - 1) / out_width);
  }
  // The source rows of one output row, summed per column and channel.
  std::vector<uint32_t> rows(size_t(width) * 3);
  for (uint32_t y = 0; y < out_height; ++y) {
    uint32_t y0 = uint32_t(uint64_t(y) * height / out_height);
    uint32_t y1 =
        uint32_t((uint64_t(y + 1) * height + out_height - 1) / out_height);
    std::fill(rows.begin(), rows.end(), 0);
    for (uint32_t sy = y0; sy < y1; ++sy) {
      const uint8_t* row = rgbx + size_t(sy) * stride;
      uint32_t* sum = rows.data();
      for (uint32_t sx = 0; sx < width; ++sx, row += 4, sum += 3) {
        sum[0] += row[0];
        sum[1] += row[1];
        sum[2] += row[2];
      }
    }
    uint8_t* pixel = out + size_t(y) * out_stride;
    for (uint32_t x = 0; x < out_width; ++x, pixel += 3) {
      std::array<uint32_t, 3> sum = {};
      for (uint32_t sx = x0s[x]; sx < x1s[x]; ++sx) {
        for (size_t c = 0; c < 3; ++c) {
          sum[c] += rows[size_t(sx) * 3 + c];
        }
      }
      uint32_t count = (y1 - y0) * (x1s[x] - x0s[x]);
      for (size_t c = 0; c < 3; ++c) {
        pixel[c] = uint8_t((sum[c] + count / 2) / count);
      }
    }
  }
}

// R8 G8 B8 X8 rows (as ui::RawImage) as R8 G8 B8, scaled down to max_width
// keeping the aspect ratio. A max_width of 0 or at least width keeps the size.
inline RgbImage ScaleToWidth(const uint8_t* rgbx, uint32_t width,
                             uint32_t height, size_t stride,
                             uint32_t max_width) {
  RgbImage image;
  if (!width || !height) {
    return image;
  }
  image.width = max_width && max_width < width ? max_width : width;
  image.height = std::max(
      uint32_t((uint64_t(height) * image.width + width / 2) / width), 1u);
  image.pixels.resize(size_t(image.width) * image.height * 3);
  ScaleInto(rgbx, width, height, stride, image.pixels.data(),
            size_t(image.width) * 3, image.width, image.height);
  return image;
}

// R8 G8 B8 X8 rows (as ui::RawImage) as exactly out_width x out_height
// R8 G8 B8, scaled up or down to fit keeping the aspect ratio, centered, with
// black bars on the other axis. Black without a source image.
inline RgbImage FitInto(const uint8_t* rgbx, uint32_t width, uint32_t height,
                        size_t stride, uint32_t out_width,
                        uint32_t out_height) {
  RgbImage image;
  image.width = out_width;
  image.height = out_height;
  image.pixels.assign(size_t(out_width) * out_height * 3, 0);
  if (!width || !height || !out_width || !out_height) {
    return image;
  }
  // Full width unless that makes it too tall, then full height.
  uint32_t fit_width = out_width;
  uint64_t fit_height =
      (uint64_t(height) * out_width + width / 2) / uint64_t(width);
  if (fit_height > out_height) {
    fit_height = out_height;
    fit_width = uint32_t(
        std::min((uint64_t(width) * out_height + height / 2) / uint64_t(height),
                 uint64_t(out_width)));
  }
  fit_width = std::max(fit_width, 1u);
  fit_height = std::max(fit_height, uint64_t(1));
  size_t x = (out_width - fit_width) / 2;
  size_t y = (out_height - size_t(fit_height)) / 2;
  size_t out_stride = size_t(out_width) * 3;
  ScaleInto(rgbx, width, height, stride,
            image.pixels.data() + y * out_stride + x * 3, out_stride, fit_width,
            uint32_t(fit_height));
  return image;
}

// An encoded frame and the guest's counters when it was captured. It shows
// swap's image, or rarely the next one's, which the GPU makes visible just
// before counting it.
struct EncodedFrame {
  FrameFormat format = FrameFormat::kJpeg;
  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t vblank = 0;
  uint64_t swap = 0;
  uint64_t ns = 0;
  telemetry::State state = telemetry::State::kStarting;
  std::vector<uint8_t> bytes;
  // Host time spent reading the guest output back, scaling and encoding.
  uint64_t capture_ns = 0;
  uint64_t scale_ns = 0;
  uint64_t encode_ns = 0;
};

// ns -> ms, 2 decimals.
inline double CostMs(uint64_t ns) {
  return std::round(double(ns) / 1e4) / 100.0;
}

// CostMs as JSON number text, for telemetry events.
inline std::string CostText(uint64_t ns) {
  uint64_t hundredths = (ns + 5000) / 10000;
  std::string fraction = std::to_string(hundredths % 100);
  return std::to_string(hundredths / 100) + (fraction.size() < 2 ? ".0" : ".") +
         fraction;
}

inline std::string FrameReply(std::string_view id, const EncodedFrame& frame) {
  return JsonLine()
      .Re(id)
      .Bool("ok", true)
      .String("format", FrameFormatName(frame.format))
      .Uint("width", frame.width)
      .Uint("height", frame.height)
      .Uint("vblank", frame.vblank)
      .Uint("swap", frame.swap)
      .Uint("ns", frame.ns)
      .Double("capture_ms", CostMs(frame.capture_ns))
      .Double("scale_ms", CostMs(frame.scale_ns))
      .Double("encode_ms", CostMs(frame.encode_ns))
      .String("data", Base64(frame.bytes.data(), frame.bytes.size()))
      .Finish();
}

// A frames_watch frame. The envelope is the capture's, not the send's.
inline std::string FrameEventLine(const EncodedFrame& frame) {
  telemetry::Event event("frame");
  event.Str("format", FrameFormatName(frame.format))
      .UInt("width", frame.width)
      .UInt("height", frame.height)
      .Raw("capture_ms", CostText(frame.capture_ns))
      .Raw("scale_ms", CostText(frame.scale_ns))
      .Raw("encode_ms", CostText(frame.encode_ns))
      .Str("data", Base64(frame.bytes.data(), frame.bytes.size()));
  return event.Finish(frame.vblank, frame.swap, frame.ns, frame.state) + "\n";
}

// Video. video_start connects to a listener on a loopback port, such as
// ffmpeg's tcp://127.0.0.1:port?listen=1, and sends it raw rgb24 frames of
// width x height at fps a second, the guest output fitted into them. A tick
// the guest hasn't swapped since repeats the last frame, so the stream keeps
// the guest's timing at a constant rate. Each new image comes with a
// video_frame event, to the client that started the stream.

// Encoders want even sizes for 4:2:0 chroma.
constexpr int64_t kMinVideoSize = 16;
constexpr int64_t kMaxVideoSize = 8192;

inline std::optional<VideoOptions> ParseVideoOptions(const Request& request,
                                                     std::string* error) {
  if (!request.port || *request.port < 1024 || *request.port > 65535) {
    *error = "port must be from 1024 to 65535";
    return std::nullopt;
  }
  for (int64_t size : {request.width.value_or(0), request.height.value_or(0)}) {
    if (size < kMinVideoSize || size > kMaxVideoSize || size % 2) {
      *error = "width and height must be even, from " +
               std::to_string(kMinVideoSize) + " to " +
               std::to_string(kMaxVideoSize);
      return std::nullopt;
    }
  }
  if (!request.fps || *request.fps < 1 || *request.fps > kMaxWatchFps) {
    *error = "fps must be from 1 to " + std::to_string(kMaxWatchFps);
    return std::nullopt;
  }
  VideoOptions options;
  options.port = uint16_t(*request.port);
  options.width = uint32_t(request.width.value_or(0));
  options.height = uint32_t(request.height.value_or(0));
  options.fps = uint32_t(*request.fps);
  return options;
}

// A stream's ticks: tick n is due at start_ns + n / fps seconds.
struct VideoClock {
  uint32_t fps = 1;
  uint64_t start_ns = 0;
  // Ticks handled, sent or dropped.
  uint64_t ticks = 0;
  // The swap of the last capture, none before the first.
  std::optional<uint64_t> last_swap;
};

inline uint64_t VideoTickNs(const VideoClock& clock, uint64_t tick) {
  return clock.start_ns + tick * 1000000000 / std::max(clock.fps, 1u);
}

// What to send now, oldest first: repeats of the last frame for the ticks
// missed, then the current tick's frame, a new capture if the guest swapped.
struct VideoStep {
  // Ticks so far behind they are skipped: the stream shortens.
  uint64_t dropped = 0;
  uint64_t repeats = 0;
  bool send = false;
  bool capture = false;
};

// The step at now_ns, with the guest at swap. A stream more than max_late
// ticks behind drops them rather than sending them all at once.
inline VideoStep NextVideoStep(VideoClock& clock, uint64_t now_ns,
                               uint64_t swap, uint64_t max_late) {
  VideoStep step;
  if (now_ns < clock.start_ns) {
    return step;
  }
  uint64_t due =
      (now_ns - clock.start_ns) * std::max(clock.fps, 1u) / 1000000000 + 1;
  if (due <= clock.ticks) {
    return step;
  }
  uint64_t late = due - clock.ticks - 1;
  if (late > max_late) {
    step.dropped = late;
  } else {
    step.repeats = late;
  }
  step.send = true;
  step.capture = clock.last_swap != swap;
  clock.last_swap = swap;
  clock.ticks = due;
  return step;
}

// start_ns: when frame 0 is due, in host ns; frame n is due n / fps seconds
// after.
inline std::string VideoStartReply(std::string_view id,
                                   const VideoOptions& options,
                                   uint64_t start_ns) {
  return JsonLine()
      .Re(id)
      .Bool("ok", true)
      .String("pix_fmt", "rgb24")
      .Uint("width", options.width)
      .Uint("height", options.height)
      .Uint("fps", options.fps)
      .Uint("ns", start_ns)
      .Finish();
}

inline std::string VideoStopReply(std::string_view id,
                                  const VideoCounts& counts) {
  return JsonLine()
      .Re(id)
      .Bool("ok", true)
      .Uint("frames", counts.frames)
      .Uint("captures", counts.captures)
      .Uint("dropped", counts.dropped)
      .Finish();
}

// A new image starting at stream frame n. The envelope is the capture's.
inline std::string VideoFrameLine(uint64_t n, const EncodedFrame& frame) {
  telemetry::Event event("video_frame");
  event.UInt("n", n).Raw("capture_ms", CostText(frame.capture_ns));
  return event.Finish(frame.vblank, frame.swap, frame.ns, frame.state) + "\n";
}

// The stream ended without a video_stop: the listener went or stopped
// reading.
inline telemetry::Event VideoEndEvent(std::string_view reason,
                                      const VideoCounts& counts) {
  telemetry::Event event("video_end");
  event.Str("reason", reason)
      .UInt("frames", counts.frames)
      .UInt("captures", counts.captures)
      .UInt("dropped", counts.dropped);
  return event;
}

// Audio. Subscribing to the audio stream taps the frames the title submits:
// each is downmixed to 16-bit stereo, and audio events carry about 100 ms of
// one client's samples, stamped with the host time of the first.

constexpr uint32_t kAudioRate = 48000;
constexpr uint32_t kGuestAudioChannels = 6;
constexpr uint32_t kGuestAudioFrameSamples = 256;
// About -3 dB, for the centre and back channels in the stereo downmix.
constexpr float kDownmixSide = 0.70710678f;
// Samples gathered before an audio event is sent.
constexpr uint32_t kAudioChunkFrames = kAudioRate / 10;
// A frame this much later than its chunk's audio would end starts a new chunk.
constexpr uint64_t kAudioGapNs = 50000000;

// One guest audio frame, kGuestAudioChannels channels of
// kGuestAudioFrameSamples big-endian floats one channel after another, in the
// order FL FR FC LFE BL BR, as interleaved 16-bit stereo:
// L = FL + 0.707 FC + 0.707 BL and R = FR + 0.707 FC + 0.707 BR, clamped. The
// LFE channel is left out. NaN reads as silence.
inline void DownmixGuestFrame(const float* samples, int16_t* stereo) {
  auto sample_at = [samples](uint32_t channel, uint32_t i) {
    uint32_t bits;
    std::memcpy(&bits, &samples[channel * kGuestAudioFrameSamples + i],
                sizeof(bits));
    bits = (bits >> 24) | ((bits >> 8) & 0xFF00) | ((bits << 8) & 0xFF0000) |
           (bits << 24);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  };
  auto to_pcm = [](float value) {
    if (!(value == value)) {
      return int16_t(0);
    }
    return int16_t(std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
  };
  for (uint32_t i = 0; i < kGuestAudioFrameSamples; ++i) {
    float centre = kDownmixSide * sample_at(2, i);
    stereo[i * 2] =
        to_pcm(sample_at(0, i) + centre + kDownmixSide * sample_at(4, i));
    stereo[i * 2 + 1] =
        to_pcm(sample_at(1, i) + centre + kDownmixSide * sample_at(5, i));
  }
}

// The media player's music (XMP) is sent as one more audio client after the
// guest's (0 to 7), with source "music".
constexpr uint32_t kMusicAudioClient = 8;

// Music frames as the guest's are sent: 16-bit stereo at kAudioRate. Music
// comes at its song's rate (often 44.1 kHz) and channel count, as interleaved
// host-endian floats; it is downmixed (mono to both sides, 5.1 and more as
// DownmixGuestFrame, otherwise the first two channels), scaled by the
// playback volume and resampled linearly. The position carries over calls, so
// consecutive frames join without a click; a new rate starts over.
class MusicResampler {
 public:
  void Convert(const float* samples, uint32_t frames, uint32_t channels,
               uint32_t rate, float volume, std::vector<int16_t>& stereo) {
    if (!frames || !channels || !rate) {
      return;
    }
    if (rate != rate_) {
      rate_ = rate;
      position_ = 0.0;
    }
    auto side = [&](uint32_t frame, bool right) {
      const float* at = samples + size_t(frame) * channels;
      float value;
      if (channels == 1) {
        value = at[0];
      } else if (channels >= 6) {
        value = at[right ? 1 : 0] + kDownmixSide * at[2] +
                kDownmixSide * at[right ? 5 : 4];
      } else {
        value = at[right ? 1 : 0];
      }
      return value == value ? value * volume : 0.0f;
    };
    auto to_pcm = [](float value) {
      return int16_t(std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
    };
    const double step = double(rate) / double(kAudioRate);
    // position_ is in input frames from this call's first; from -1 to 0 it
    // falls between the last frame of the call before and this one's first.
    const auto last = int64_t(frames) - 1;
    while (position_ <= double(last)) {
      auto index = int64_t(std::floor(position_));
      float fraction = float(position_ - double(index));
      for (bool right : {false, true}) {
        float a = index < 0 ? (right ? last_right_ : last_left_)
                            : side(uint32_t(index), right);
        float b = side(uint32_t(std::min(index + 1, last)), right);
        stereo.push_back(to_pcm(a + (b - a) * fraction));
      }
      position_ += step;
    }
    // Past this call's last frame, so from -1 on in the next.
    position_ -= double(frames);
    last_left_ = side(uint32_t(last), false);
    last_right_ = side(uint32_t(last), true);
  }

 private:
  uint32_t rate_ = 0;
  double position_ = 0.0;
  float last_left_ = 0.0f;
  float last_right_ = 0.0f;
};

// When music frames play. The media player hands its driver a queue's worth
// of frames at once when a song starts, then one each time one finishes, so
// the time a frame is handed over runs up to a queue (about a second) early.
// A frame plays when the one before it ends, or now if the queue ran dry.
class MusicClock {
 public:
  // The host time the next `frames` frames at kAudioRate start playing.
  uint64_t Stamp(uint64_t now_ns, uint64_t frames) {
    if (next_ns_ < now_ns) {
      next_ns_ = now_ns;
    }
    uint64_t at = next_ns_;
    next_ns_ += frames * 1000000000 / kAudioRate;
    return at;
  }

 private:
  uint64_t next_ns_ = 0;
};

// Contiguous stereo samples of one audio client.
struct AudioChunk {
  uint32_t client = 0;
  // Host time and guest counters when its first frame was submitted.
  uint64_t ns = 0;
  uint64_t vblank = 0;
  uint64_t swap = 0;
  // Interleaved L R.
  std::vector<int16_t> samples;
  uint64_t frames() const { return samples.size() / 2; }
};

// Gathers each client's downmixed frames into chunks of at least min_frames.
class AudioChunker {
 public:
  explicit AudioChunker(uint64_t min_frames = kAudioChunkFrames,
                        uint64_t max_gap_ns = kAudioGapNs)
      : min_frames_(min_frames), max_gap_ns_(max_gap_ns) {}

  // frames of interleaved stereo submitted by client at ns; full chunks go to
  // done.
  void Add(uint32_t client, uint64_t ns, uint64_t vblank, uint64_t swap,
           const int16_t* stereo, size_t frames,
           std::vector<AudioChunk>& done) {
    Pending& pending = Find(client);
    AudioChunk& chunk = pending.chunk;
    if (!chunk.samples.empty() &&
        ns >
            chunk.ns + chunk.frames() * 1000000000 / kAudioRate + max_gap_ns_) {
      done.push_back(std::move(chunk));
      chunk = AudioChunk();
    }
    if (chunk.samples.empty()) {
      chunk.client = client;
      chunk.ns = ns;
      chunk.vblank = vblank;
      chunk.swap = swap;
    }
    chunk.samples.insert(chunk.samples.end(), stereo, stereo + frames * 2);
    pending.last_ns = ns;
    if (chunk.frames() >= min_frames_) {
      done.push_back(std::move(chunk));
      chunk = AudioChunk();
    }
  }

  // Ends the chunks whose last frame came max_age_ns or more before now_ns.
  void Flush(uint64_t now_ns, uint64_t max_age_ns,
             std::vector<AudioChunk>& done) {
    for (Pending& pending : pending_) {
      if (!pending.chunk.samples.empty() &&
          now_ns >= pending.last_ns + max_age_ns) {
        done.push_back(std::move(pending.chunk));
        pending.chunk = AudioChunk();
      }
    }
  }

  void Clear() { pending_.clear(); }

 private:
  struct Pending {
    uint32_t client = 0;
    AudioChunk chunk;
    uint64_t last_ns = 0;
  };
  Pending& Find(uint32_t client) {
    auto it = std::find_if(
        pending_.begin(), pending_.end(),
        [client](const Pending& pending) { return pending.client == client; });
    if (it != pending_.end()) {
      return *it;
    }
    pending_.emplace_back();
    pending_.back().client = client;
    return pending_.back();
  }

  uint64_t min_frames_;
  uint64_t max_gap_ns_;
  std::vector<Pending> pending_;
};

// An audio event: chunk's samples as base64 little-endian 16-bit stereo.
inline std::string AudioEventLine(const AudioChunk& chunk,
                                  telemetry::State state) {
  std::vector<uint8_t> bytes;
  bytes.reserve(chunk.samples.size() * 2);
  for (int16_t sample : chunk.samples) {
    bytes.push_back(uint8_t(uint16_t(sample) & 0xFF));
    bytes.push_back(uint8_t(uint16_t(sample) >> 8));
  }
  telemetry::Event event("audio");
  event.UInt("client", chunk.client)
      .Str("source", chunk.client == kMusicAudioClient ? "music" : "game")
      .UInt("rate", kAudioRate)
      .UInt("channels", 2)
      .UInt("frames", chunk.frames())
      .Str("data", Base64(bytes.data(), bytes.size()));
  return event.Finish(chunk.vblank, chunk.swap, chunk.ns, state) + "\n";
}

// Benching. bench_start gathers the frame times telemetry writes from then on;
// bench_stop summarizes them as a frame time review would: average fps, the
// 1% and 0.1% lows, percentiles and stutters, and the same for host presents.

// Adds a frame_stats telemetry line to samples; other lines are ignored.
inline void AddFrameStats(BenchSamples& samples, std::string_view line) {
  if (!line.starts_with(R"({"type":"frame_stats")")) {
    return;
  }
  rapidjson::Document stats;
  stats.Parse(line.data(), line.size());
  if (stats.HasParseError() || !stats.IsObject()) {
    return;
  }
  auto uint_of = [&stats](const char* key) -> uint64_t {
    auto member = stats.FindMember(key);
    return member != stats.MemberEnd() && member->value.IsUint64()
               ? member->value.GetUint64()
               : 0;
  };
  auto append = [&stats](const char* key, auto& values) {
    auto member = stats.FindMember(key);
    if (member == stats.MemberEnd() || !member->value.IsArray()) {
      return;
    }
    using Value = typename std::decay_t<decltype(values)>::value_type;
    for (const auto& value : member->value.GetArray()) {
      if constexpr (std::is_floating_point_v<Value>) {
        if (value.IsNumber()) {
          values.push_back(value.GetDouble());
        }
      } else if (value.IsUint64()) {
        values.push_back(value.GetUint64());
      }
    }
  };
  append("swap_ms", samples.swap_ms);
  append("swap_compiles", samples.swap_compiles);
  append("present_ms", samples.present_ms);
  append("present_latency_ms", samples.present_latency_ms);
  uint64_t ns = uint_of("ns");
  if (samples.windows == 0) {
    samples.first_ns = ns;
  } else {
    samples.vblanks_after_first += uint_of("window_vblanks");
  }
  samples.last_ns = ns;
  ++samples.windows;
}

// 2 decimals.
inline double Round2(double value) { return std::round(value * 100.0) / 100.0; }

// The nearest-rank percentile of sorted values, which must not be empty.
inline double Percentile(const std::vector<double>& sorted, double percent) {
  size_t rank = size_t(std::ceil(percent / 100.0 * double(sorted.size())));
  return sorted[std::clamp(rank, size_t(1), sorted.size()) - 1];
}

struct FrameTimeSummary {
  size_t frames = 0;
  double avg_fps = 0;
  // 1000 / the mean of the slowest 1% and 0.1% of frame times, at least one.
  double low_1_fps = 0;
  double low_0_1_fps = 0;
  double p50_ms = 0;
  double p95_ms = 0;
  double p99_ms = 0;
  double max_ms = 0;
};

inline FrameTimeSummary SummarizeFrameTimes(std::vector<double> ms) {
  FrameTimeSummary summary;
  summary.frames = ms.size();
  if (ms.empty()) {
    return summary;
  }
  std::sort(ms.begin(), ms.end());
  double total = std::accumulate(ms.begin(), ms.end(), 0.0);
  auto low = [&ms](double fraction) {
    size_t count = std::max(
        size_t(1), size_t(std::ceil(fraction * double(ms.size()) - 1e-9)));
    double sum = 0;
    for (size_t i = ms.size() - count; i < ms.size(); ++i) {
      sum += ms[i];
    }
    return sum > 0 ? 1000.0 * double(count) / sum : 0.0;
  };
  summary.avg_fps = total > 0 ? 1000.0 * double(ms.size()) / total : 0;
  summary.low_1_fps = low(0.01);
  summary.low_0_1_fps = low(0.001);
  summary.p50_ms = Percentile(ms, 50);
  summary.p95_ms = Percentile(ms, 95);
  summary.p99_ms = Percentile(ms, 99);
  summary.max_ms = ms.back();
  return summary;
}

inline std::string FrameTimeSummaryJson(const FrameTimeSummary& summary) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writer.StartObject();
  writer.Key("frames");
  writer.Uint64(summary.frames);
  for (auto [key, value] : {std::pair{"avg_fps", summary.avg_fps},
                            std::pair{"low_1_fps", summary.low_1_fps},
                            std::pair{"low_0_1_fps", summary.low_0_1_fps},
                            std::pair{"p50_ms", summary.p50_ms},
                            std::pair{"p95_ms", summary.p95_ms},
                            std::pair{"p99_ms", summary.p99_ms},
                            std::pair{"max_ms", summary.max_ms}}) {
    writer.Key(key);
    writer.Double(Round2(value));
  }
  writer.EndObject();
  return std::string(buffer.GetString(), buffer.GetSize());
}

// A stutter: a frame time at least twice the median.
struct Stutters {
  size_t count = 0;
  // Of those, the ones with a shader or pipeline compile in their interval.
  size_t with_compiles = 0;
};

inline Stutters CountStutters(const std::vector<double>& ms,
                              const std::vector<uint64_t>& compiles) {
  Stutters stutters;
  if (ms.empty()) {
    return stutters;
  }
  std::vector<double> sorted = ms;
  std::sort(sorted.begin(), sorted.end());
  double threshold = 2 * Percentile(sorted, 50);
  for (size_t i = 0; i < ms.size(); ++i) {
    if (ms[i] >= threshold) {
      ++stutters.count;
      if (i < compiles.size() && compiles[i]) {
        ++stutters.with_compiles;
      }
    }
  }
  return stutters;
}

template <typename T>
std::string NumberListJson(const std::vector<T>& values) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writer.StartArray();
  for (T value : values) {
    if constexpr (std::is_floating_point_v<T>) {
      writer.Double(value);
    } else {
      writer.Uint64(value);
    }
  }
  writer.EndArray();
  return std::string(buffer.GetString(), buffer.GetSize());
}

inline std::string BenchReply(std::string_view id, const BenchSamples& samples,
                              bool raw) {
  JsonLine line;
  line.Re(id).Bool("ok", true).Uint("windows", samples.windows);
  if (samples.windows >= 2 && samples.last_ns > samples.first_ns) {
    double seconds = double(samples.last_ns - samples.first_ns) / 1e9;
    line.Double("seconds", Round2(seconds))
        .Double("vblank_hz",
                Round2(double(samples.vblanks_after_first) / seconds));
  }
  line.Raw("swaps", FrameTimeSummaryJson(SummarizeFrameTimes(samples.swap_ms)));
  Stutters stutters = CountStutters(samples.swap_ms, samples.swap_compiles);
  line.Uint("stutters", stutters.count)
      .Uint("stutters_with_compiles", stutters.with_compiles);
  line.Uint("compiles",
            std::accumulate(samples.swap_compiles.begin(),
                            samples.swap_compiles.end(), uint64_t(0)));
  if (!samples.present_ms.empty() || !samples.present_latency_ms.empty()) {
    line.Raw("presents",
             FrameTimeSummaryJson(SummarizeFrameTimes(samples.present_ms)));
    std::vector<double> latency = samples.present_latency_ms;
    std::sort(latency.begin(), latency.end());
    if (!latency.empty()) {
      line.Double("present_latency_p50_ms", Round2(Percentile(latency, 50)))
          .Double("present_latency_p99_ms", Round2(Percentile(latency, 99)));
    }
  }
  if (raw) {
    line.Raw("swap_ms", NumberListJson(samples.swap_ms))
        .Raw("swap_compiles", NumberListJson(samples.swap_compiles))
        .Raw("present_ms", NumberListJson(samples.present_ms))
        .Raw("present_latency_ms", NumberListJson(samples.present_latency_ms));
  }
  return line.Finish();
}

// Sent to every client before the process ends at a client's request.
inline std::string ExitEvent(int status) {
  return JsonLine().String("type", "exit").Int("status", status).Finish();
}

// Threads. The threads op reports what every guest thread is doing, for
// looking into hangs: each one's wait (the objects, how long, and the guest
// return address and stack at the call) and what the GPU waits on.

// Return addresses on a guest stack, innermost first: the first is lr, the
// return address of the call the thread is in; then the saved return address
// of each frame up the back chain. Xbox 360 code saves a function's return
// address at -8 from the stack pointer it was called with, which is the back
// chain word of its own frame. The walk stays within [limit, base), the
// thread's stack, and ends at a chain that doesn't climb or a return address
// that is_code refuses, outside the title's modules (past the outermost frame,
// an unused stack holds fill bytes).
template <typename Read32, typename IsCode>
std::vector<uint32_t> WalkGuestStack(Read32 read32, IsCode is_code, uint32_t lr,
                                     uint32_t sp, uint32_t limit, uint32_t base,
                                     size_t max_frames) {
  std::vector<uint32_t> stack;
  if (!is_code(lr)) {
    return stack;
  }
  stack.push_back(lr);
  while (stack.size() < max_frames && sp >= limit && sp < base && sp % 4 == 0) {
    uint32_t back = read32(sp);
    if (back <= sp || back > base || back - 8 < limit) {
      break;
    }
    uint32_t saved_lr = read32(back - 8);
    if (!is_code(saved_lr)) {
      break;
    }
    stack.push_back(saved_lr);
    sp = back;
  }
  return stack;
}

struct WaitedObject {
  uint32_t handle = 0;
  std::string type;
  std::string name;
  // A mutant's owner, when a guest thread holds it.
  std::optional<uint32_t> owner_thread;
};

struct ThreadSnapshot {
  uint32_t id = 0;
  uint32_t handle = 0;
  std::string name;
  int32_t priority = 0;
  uint32_t suspend_count = 0;
  // Empty when it isn't waiting; else one, any, all, signal_and_wait or delay.
  std::string wait;
  // The first objects waited on, of object_count.
  std::vector<WaitedObject> objects;
  uint32_t object_count = 0;
  std::optional<uint64_t> timeout_ms;
  uint64_t waited_ms = 0;
  std::vector<uint32_t> stack;
};

// A loaded title module (the executable or one of its DLLs): where its image
// is, for reading return addresses as module and offset.
struct ModuleSnapshot {
  std::string name;
  uint32_t base = 0;
  uint32_t size = 0;
};

inline bool InModules(const std::vector<ModuleSnapshot>& modules,
                      uint32_t address) {
  return address % 4 == 0 &&
         std::any_of(modules.begin(), modules.end(),
                     [address](const ModuleSnapshot& module) {
                       return address >= module.base &&
                              address - module.base < module.size;
                     });
}

struct GpuWaitSnapshot {
  bool is_memory = false;
  uint32_t address = 0;
  uint32_t ref = 0;
  uint32_t mask = 0;
  uint32_t function = 0;
  uint64_t waited_ms = 0;
};

inline std::string Hex32(uint32_t value) {
  static constexpr char kDigits[] = "0123456789ABCDEF";
  std::string text = "0x00000000";
  for (int i = 0; i < 8; ++i) {
    text[9 - i] = kDigits[(value >> (i * 4)) & 0xF];
  }
  return text;
}

inline std::string ThreadsReply(std::string_view id,
                                const std::vector<ThreadSnapshot>& threads,
                                const std::vector<ModuleSnapshot>& modules,
                                const std::optional<GpuWaitSnapshot>& gpu,
                                uint64_t vblank, uint64_t swap) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  auto string = [&writer](const char* key, std::string_view value) {
    writer.Key(key);
    writer.String(value.data(), rapidjson::SizeType(value.size()));
  };
  writer.StartObject();
  if (!id.empty()) {
    writer.Key("re");
    writer.RawValue(id.data(), id.size(), rapidjson::kNumberType);
  }
  writer.Key("ok");
  writer.Bool(true);
  writer.Key("vblank");
  writer.Uint64(vblank);
  writer.Key("swap");
  writer.Uint64(swap);
  writer.Key("threads");
  writer.StartArray();
  for (const ThreadSnapshot& thread : threads) {
    writer.StartObject();
    writer.Key("id");
    writer.Uint(thread.id);
    string("handle", Hex32(thread.handle));
    string("name", thread.name);
    writer.Key("priority");
    writer.Int(thread.priority);
    writer.Key("suspended");
    writer.Uint(thread.suspend_count);
    if (!thread.wait.empty()) {
      string("wait", thread.wait);
      writer.Key("waited_ms");
      writer.Uint64(thread.waited_ms);
      writer.Key("timeout_ms");
      if (thread.timeout_ms) {
        writer.Uint64(*thread.timeout_ms);
      } else {
        writer.Null();
      }
      writer.Key("object_count");
      writer.Uint(thread.object_count);
      writer.Key("objects");
      writer.StartArray();
      for (const WaitedObject& object : thread.objects) {
        writer.StartObject();
        string("handle", Hex32(object.handle));
        string("type", object.type);
        if (!object.name.empty()) {
          string("name", object.name);
        }
        if (object.owner_thread) {
          writer.Key("owner");
          writer.Uint(*object.owner_thread);
        }
        writer.EndObject();
      }
      writer.EndArray();
      writer.Key("stack");
      writer.StartArray();
      for (uint32_t address : thread.stack) {
        std::string text = Hex32(address);
        writer.String(text.data(), rapidjson::SizeType(text.size()));
      }
      writer.EndArray();
    }
    writer.EndObject();
  }
  writer.EndArray();
  writer.Key("modules");
  writer.StartArray();
  for (const ModuleSnapshot& module : modules) {
    writer.StartObject();
    string("name", module.name);
    string("base", Hex32(module.base));
    string("size", Hex32(module.size));
    writer.EndObject();
  }
  writer.EndArray();
  writer.Key("gpu_wait");
  if (gpu) {
    writer.StartObject();
    string("on", gpu->is_memory ? "memory" : "register");
    string("address", Hex32(gpu->address));
    string("ref", Hex32(gpu->ref));
    string("mask", Hex32(gpu->mask));
    writer.Key("function");
    writer.Uint(gpu->function);
    writer.Key("waited_ms");
    writer.Uint64(gpu->waited_ms);
    writer.EndObject();
  } else {
    writer.Null();
  }
  writer.EndObject();
  return std::string(buffer.GetString(), buffer.GetSize()) + "\n";
}

}  // namespace automation
}  // namespace xe

#endif  // XENIA_AUTOMATION_PROTOCOL_H_
