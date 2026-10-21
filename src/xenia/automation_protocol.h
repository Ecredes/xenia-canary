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

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "third_party/rapidjson/include/rapidjson/document.h"
#include "third_party/rapidjson/include/rapidjson/stringbuffer.h"
#include "third_party/rapidjson/include/rapidjson/writer.h"

// The automation channel's protocol, kept free of sockets and threads: JSON
// lines in, decisions and JSON lines out. AutomationChannel carries them out.
// See docs/automation.md for the protocol.

namespace xe {
namespace automation {

constexpr uint32_t kProtocolVersion = 1;
// A longer line drops the client.
constexpr size_t kMaxLineLength = 16 * 1024 * 1024;

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

// One connection's state.
struct Session {
  Role role = Role::kNone;
};

enum class Action {
  kNone,
  // Send the reply, then close this connection.
  kClose,
  // Send the reply and the exit event, then end the process.
  kStop,
};

struct Decision {
  std::string reply;
  Action action = Action::kNone;
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
                .Raw("capabilities", R"(["state","stop"])")
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
  if (request.op == "stop") {
    if (session.role != Role::kController) {
      return {ErrorLine(request.id, "needs the controller role")};
    }
    return {JsonLine().Re(request.id).Bool("ok", true).Finish(), Action::kStop};
  }
  return {ErrorLine(request.id, "unknown op")};
}

// Sent to every client before the process ends at a client's request.
inline std::string ExitEvent(int status) {
  return JsonLine().String("type", "exit").Int("status", status).Finish();
}

}  // namespace automation
}  // namespace xe

#endif  // XENIA_AUTOMATION_PROTOCOL_H_
