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
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
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
        std::pair{"quality", &request.quality},
        std::pair{"fps", &request.fps}}) {
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

// One connection's state.
struct Session {
  Role role = Role::kNone;
  // Subscribed to telemetry events.
  bool telemetry = false;
  // Subscribed to dialog events; the controller answers them.
  bool dialogs = false;
  // Subscribed to pad events.
  bool input = false;
  // Watching frames.
  std::optional<FrameWatch> frames;
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
};

struct Decision {
  std::string reply;
  Action action = Action::kNone;
  std::optional<hid::remote::Command> pad;
  FrameOptions frame_options;
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
                     R"("frame","frames_watch"])")
                .Raw("streams", R"(["telemetry","dialogs","input"])")
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
    subscribed.telemetry = subscribed.dialogs = subscribed.input = false;
    for (const std::string& stream : *request.streams) {
      if (stream == "telemetry") {
        subscribed.telemetry = true;
      } else if (stream == "dialogs") {
        subscribed.dialogs = true;
      } else if (stream == "input") {
        subscribed.input = true;
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

// R8 G8 B8 X8 rows (as ui::RawImage) as R8 G8 B8, scaled down to max_width
// keeping the aspect ratio, each pixel the average of the source pixels it
// touches. A max_width of 0 or at least width keeps the size.
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
  for (uint32_t y = 0; y < image.height; ++y) {
    // Never upscaled, so each output pixel touches at least one source pixel.
    uint32_t y0 = uint32_t(uint64_t(y) * height / image.height);
    uint32_t y1 =
        uint32_t((uint64_t(y + 1) * height + image.height - 1) / image.height);
    for (uint32_t x = 0; x < image.width; ++x) {
      uint32_t x0 = uint32_t(uint64_t(x) * width / image.width);
      uint32_t x1 =
          uint32_t((uint64_t(x + 1) * width + image.width - 1) / image.width);
      std::array<uint32_t, 3> sum = {};
      for (uint32_t sy = y0; sy < y1; ++sy) {
        const uint8_t* row = rgbx + sy * stride;
        for (uint32_t sx = x0; sx < x1; ++sx) {
          for (size_t c = 0; c < 3; ++c) {
            sum[c] += row[sx * 4 + c];
          }
        }
      }
      uint32_t count = (y1 - y0) * (x1 - x0);
      uint8_t* out = &image.pixels[(size_t(y) * image.width + x) * 3];
      for (size_t c = 0; c < 3; ++c) {
        out[c] = uint8_t((sum[c] + count / 2) / count);
      }
    }
  }
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
};

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
      .String("data", Base64(frame.bytes.data(), frame.bytes.size()))
      .Finish();
}

// A frames_watch frame. The envelope is the capture's, not the send's.
inline std::string FrameEventLine(const EncodedFrame& frame) {
  telemetry::Event event("frame");
  event.Str("format", FrameFormatName(frame.format))
      .UInt("width", frame.width)
      .UInt("height", frame.height)
      .Str("data", Base64(frame.bytes.data(), frame.bytes.size()));
  return event.Finish(frame.vblank, frame.swap, frame.ns, frame.state) + "\n";
}

// Sent to every client before the process ends at a client's request.
inline std::string ExitEvent(int status) {
  return JsonLine().String("type", "exit").Int("status", status).Finish();
}

}  // namespace automation
}  // namespace xe

#endif  // XENIA_AUTOMATION_PROTOCOL_H_
