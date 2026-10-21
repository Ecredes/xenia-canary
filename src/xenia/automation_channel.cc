/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/automation_channel.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <random>
#include <utility>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/telemetry.h"
#include "xenia/base/threading.h"
#include "xenia/emulator.h"
#include "xenia/gpu/graphics_system.h"

#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
// winsock includes must come after platform_win.h:
#include <winsock2.h>  // NOLINT(build/include_order)
#include <ws2tcpip.h>  // NOLINT(build/include_order)
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#endif

DEFINE_transient_int32(
    automation_port, 0,
    "Listen for an automation client on this loopback TCP port, 0 for none. "
    "See docs/automation.md.",
    "Automation");
DEFINE_transient_int32(
    automation_dialog_timeout, 30,
    "Seconds an automation client subscribed to dialogs has to answer one "
    "before the default answers it.",
    "Automation");
DEFINE_transient_string(
    automation_token, "",
    "The token an automation client must send. Without one, a random token "
    "is written to automation.token in the storage root.",
    "Automation");

namespace xe {
namespace automation {

namespace {

#if XE_PLATFORM_WIN32
constexpr SocketHandle kInvalidSocket = SocketHandle(INVALID_SOCKET);
void CloseSocket(SocketHandle socket) { closesocket(SOCKET(socket)); }
bool WouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
bool SetNonBlocking(SocketHandle socket) {
  u_long enabled = 1;
  return ioctlsocket(SOCKET(socket), FIONBIO, &enabled) == 0;
}
int SendSome(SocketHandle socket, const char* data, size_t size) {
  return send(SOCKET(socket), data, int(size), 0);
}
int ReceiveSome(SocketHandle socket, char* data, size_t size) {
  return recv(SOCKET(socket), data, int(size), 0);
}
#else
constexpr SocketHandle kInvalidSocket = -1;
void CloseSocket(SocketHandle socket) { close(socket); }
bool WouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }
bool SetNonBlocking(SocketHandle socket) {
  int flags = fcntl(socket, F_GETFL, 0);
  return flags != -1 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}
int SendSome(SocketHandle socket, const char* data, size_t size) {
#ifdef MSG_NOSIGNAL
  // A client gone mid-send must not end the process with SIGPIPE.
  return int(send(socket, data, size, MSG_NOSIGNAL));
#else
  return int(send(socket, data, size, 0));
#endif
}
int ReceiveSome(SocketHandle socket, char* data, size_t size) {
  return int(recv(socket, data, size, 0));
}
#endif

constexpr size_t kMaxClients = 8;
constexpr int32_t kPollMicroseconds = 50000;
// How long the exit event may take to reach the clients.
constexpr auto kStopFlushTime = std::chrono::seconds(1);
// A client this far behind is dropped rather than buffered for.
constexpr size_t kMaxClientBacklog = 16 * 1024 * 1024;
// Telemetry lines kept for the channel thread between its rounds.
constexpr size_t kMaxQueuedTelemetry = 65536;

// std::random_device draws from the OS's secure source with MSVC, libstdc++
// and libc++.
std::string NewToken() {
  std::random_device device;
  std::array<uint8_t, 16> bytes;
  std::generate(bytes.begin(), bytes.end(),
                [&device]() { return uint8_t(device()); });
  return TokenFromBytes(bytes);
}

// Readable only by the user, as far as the platform allows: on Windows the
// storage root is in the user's profile unless portable.
bool WriteTokenFile(const std::filesystem::path& path,
                    const std::string& token) {
#if XE_PLATFORM_WIN32
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << token;
  return file.good();
#else
  // A symlink planted at the path must not redirect the write.
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
  if (fd < 0) {
    return false;
  }
  // An existing file keeps its mode through O_CREAT.
  bool ok = fchmod(fd, 0600) == 0 &&
            write(fd, token.data(), token.size()) == ssize_t(token.size());
  close(fd);
  return ok;
#endif
}

}  // namespace

class AutomationChannel::TelemetrySink : public telemetry::Sink {
 public:
  explicit TelemetrySink(std::shared_ptr<TelemetryFeed> feed)
      : feed_(std::move(feed)) {}
  void Write(std::string_view line) override {
    std::lock_guard<std::mutex> lock(feed_->mutex);
    if (feed_->session_start.empty() && IsSessionStart(line)) {
      feed_->session_start = line;
    }
    if (feed_->lines.size() < kMaxQueuedTelemetry) {
      feed_->lines.emplace_back(line);
    }
  }

 private:
  std::shared_ptr<TelemetryFeed> feed_;
};

std::unique_ptr<AutomationChannel> AutomationChannel::Create(
    Emulator& emulator, BuildInfo build, std::function<void()> stop) {
  int32_t port = cvars::automation_port;
  if (port <= 0) {
    return nullptr;
  }
  if (port > 65535) {
    XELOGE("Automation: port {} is out of range", port);
    return nullptr;
  }
#if XE_PLATFORM_WIN32
  WSADATA wsa_data;
  if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
    XELOGE("Automation: WSAStartup failed");
    return nullptr;
  }
#endif
  SocketHandle listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  bool listening = listen_socket != kInvalidSocket;
  if (listening) {
#if XE_PLATFORM_WIN32
    // No other process may take the port over while we listen.
    BOOL exclusive = TRUE;
    setsockopt(SOCKET(listen_socket), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#else
    // A previous run's connections in TIME_WAIT must not keep the port.
    // Unlike on Windows, this doesn't let another socket listen on it too.
    int reuse = 1;
    setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(uint16_t(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    listening = bind(listen_socket, reinterpret_cast<sockaddr*>(&address),
                     sizeof(address)) == 0 &&
                listen(listen_socket, int(kMaxClients)) == 0 &&
                SetNonBlocking(listen_socket);
  }
  if (!listening) {
    XELOGE("Automation: unable to listen on 127.0.0.1:{}", port);
    if (listen_socket != kInvalidSocket) {
      CloseSocket(listen_socket);
    }
#if XE_PLATFORM_WIN32
    WSACleanup();
#endif
    return nullptr;
  }

  std::string token = cvars::automation_token;
  if (token.empty()) {
    token = NewToken();
    std::filesystem::path token_path =
        emulator.storage_root() / "automation.token";
    if (!WriteTokenFile(token_path, token)) {
      XELOGE("Automation: unable to write the token to {}", token_path);
      CloseSocket(listen_socket);
#if XE_PLATFORM_WIN32
      WSACleanup();
#endif
      return nullptr;
    }
    XELOGI("Automation: the token is in {}", token_path);
  }

  auto snapshot = std::make_shared<SnapshotSource>();
  emulator.on_launch.AddListener(
      [snapshot](uint32_t title_id, std::string_view title_name) {
        std::lock_guard<std::mutex> lock(snapshot->mutex);
        snapshot->snapshot = {true, title_id, std::string(title_name)};
      });
  emulator.on_terminate.AddListener([snapshot]() {
    std::lock_guard<std::mutex> lock(snapshot->mutex);
    snapshot->snapshot = {};
  });

  // Telemetry streams to subscribers whether or not it also goes to a file.
  auto telemetry_feed = std::make_shared<TelemetryFeed>();
  telemetry::AddSink(std::make_unique<TelemetrySink>(telemetry_feed));
  telemetry::Start();

  // Pads time their states by guest vblanks, from the graphics system once
  // the emulator is set up.
  auto pads = std::make_shared<hid::remote::RemotePads>([&emulator]() {
    hid::remote::FrameCounters counters;
    if (gpu::GraphicsSystem* graphics_system = emulator.graphics_system()) {
      counters.vblank = graphics_system->vblank_count();
      counters.swap = graphics_system->swap_count();
    }
    return counters;
  });

  XELOGI("Automation: listening on 127.0.0.1:{}", port);
  return std::unique_ptr<AutomationChannel>(new AutomationChannel(
      listen_socket, std::move(token), std::move(build), std::move(snapshot),
      std::move(telemetry_feed), std::move(pads), std::move(stop)));
}

AutomationChannel::AutomationChannel(
    SocketHandle listen_socket, std::string token, BuildInfo build,
    std::shared_ptr<SnapshotSource> snapshot,
    std::shared_ptr<TelemetryFeed> telemetry,
    std::shared_ptr<hid::remote::RemotePads> pads, std::function<void()> stop)
    : listen_socket_(listen_socket),
      token_(std::move(token)),
      build_(std::move(build)),
      snapshot_(std::move(snapshot)),
      telemetry_(std::move(telemetry)),
      pads_(std::move(pads)),
      stop_(std::move(stop)),
      dialog_timeout_(
          std::chrono::seconds(std::max(cvars::automation_dialog_timeout, 1))) {
  thread_ = std::thread([this]() { ServerLoop(); });
}

AutomationChannel::~AutomationChannel() {
  running_ = false;
  if (thread_.joinable()) {
    thread_.join();
  }
  controller_answers_ = false;
  CloseAllDialogs();
  for (const auto& client : clients_) {
    CloseSocket(client->socket);
  }
  CloseSocket(listen_socket_);
#if XE_PLATFORM_WIN32
  WSACleanup();
#endif
}

void AutomationChannel::ServerLoop() {
  xe::threading::set_name("Automation Channel");
  while (running_) {
    fd_set read_set, write_set;
    FD_ZERO(&read_set);
    FD_ZERO(&write_set);
    FD_SET(listen_socket_, &read_set);
    SocketHandle max_socket = listen_socket_;
    for (const auto& client : clients_) {
      if (!client->closing) {
        FD_SET(client->socket, &read_set);
      }
      if (!client->out.empty()) {
        FD_SET(client->socket, &write_set);
      }
      max_socket = std::max(max_socket, client->socket);
    }
    timeval timeout = {0, kPollMicroseconds};
    if (select(int(max_socket + 1), &read_set, &write_set, nullptr, &timeout) <
        0) {
      // Interrupted; the next round starts over.
      continue;
    }
    if (FD_ISSET(listen_socket_, &read_set)) {
      Accept();
    }
    ForwardTelemetry();
    ServiceDialogs();
    ForwardPadEvents();
    for (auto it = clients_.begin(); it != clients_.end();) {
      Client& client = **it;
      bool alive = true;
      if (FD_ISSET(client.socket, &read_set)) {
        alive = Read(client);
      }
      if (alive && !client.out.empty()) {
        alive = Flush(client);
      }
      if (alive && client.closing && client.out.empty()) {
        alive = false;
      }
      if (alive) {
        ++it;
      } else {
        if (client.session.role == Role::kController) {
          // No one drives the pads any more: the guest sees them unplugged.
          pads_->Reset();
        }
        CloseSocket(client.socket);
        it = clients_.erase(it);
      }
    }
    if (stopping_) {
      Stop();
    }
  }
}

void AutomationChannel::Accept() {
  while (true) {
    SocketHandle socket = accept(listen_socket_, nullptr, nullptr);
    if (socket == kInvalidSocket) {
      return;
    }
    if (clients_.size() >= kMaxClients || !SetNonBlocking(socket)) {
      std::string error = ErrorLine("", "too many clients");
      SendSome(socket, error.data(), error.size());
      CloseSocket(socket);
      continue;
    }
    auto client = std::make_unique<Client>();
    client->socket = socket;
    clients_.push_back(std::move(client));
  }
}

bool AutomationChannel::Read(Client& client) {
  std::array<char, 4096> buffer;
  while (true) {
    int received = ReceiveSome(client.socket, buffer.data(), buffer.size());
    if (received == 0) {
      return false;
    }
    if (received < 0) {
      if (WouldBlock()) {
        break;
      }
      return false;
    }
    client.splitter.Append(std::string_view(buffer.data(), received));
  }
  while (!client.closing && !stopping_) {
    std::optional<std::string> line = client.splitter.NextLine();
    if (!line) {
      break;
    }
    Handle(client, *line);
  }
  return !client.splitter.overflowed();
}

bool AutomationChannel::Flush(Client& client) {
  while (!client.out.empty()) {
    int sent = SendSome(client.socket, client.out.data(), client.out.size());
    if (sent < 0) {
      return WouldBlock();
    }
    client.out.erase(0, size_t(sent));
  }
  return true;
}

void AutomationChannel::Handle(Client& client, std::string_view line) {
  ParseResult parsed = ParseRequest(line);
  if (!parsed.request) {
    client.out += ErrorLine(parsed.id, parsed.error);
    // Before hello, anything unreadable ends the connection.
    client.closing = client.session.role == Role::kNone;
    return;
  }
  Context context;
  context.token = token_;
  context.build = build_;
  for (const auto& other : clients_) {
    if (other->session.role == Role::kController) {
      context.controller_free = false;
    }
  }
  {
    std::lock_guard<std::mutex> lock(snapshot_->mutex);
    context.snapshot = snapshot_->snapshot;
  }
  Role role_before = client.session.role;
  bool had_telemetry = client.session.telemetry;
  bool had_dialogs = client.session.dialogs;
  Decision decision = Decide(client.session, *parsed.request, context);
  client.out += decision.reply;
  if (role_before == Role::kNone && client.session.role != Role::kNone) {
    XELOGI("Automation: a client connected as {}",
           RoleName(client.session.role));
  }
  if (!had_telemetry && client.session.telemetry) {
    std::lock_guard<std::mutex> lock(telemetry_->mutex);
    if (!telemetry_->session_start.empty()) {
      client.out += telemetry_->session_start + "\n";
    }
  }
  if (!had_dialogs && client.session.dialogs) {
    std::lock_guard<std::mutex> lock(dialogs_mutex_);
    for (const auto& [id, dialog] : open_dialogs_) {
      client.out += dialog.event + "\n";
    }
  }
  switch (decision.action) {
    case Action::kNone:
      break;
    case Action::kClose:
      client.closing = true;
      if (role_before == Role::kNone) {
        XELOGW("Automation: refused a client: {}", decision.reply);
      }
      break;
    case Action::kAnswerDialog: {
      // Decide checked that the request names a dialog.
      if (!parsed.request->dialog) {
        break;
      }
      std::function<void(DialogAnswer)> done;
      std::optional<DialogAnswer> answer;
      {
        std::lock_guard<std::mutex> lock(dialogs_mutex_);
        auto it = open_dialogs_.find(*parsed.request->dialog);
        DialogAnswerDecision answered = DecideDialogAnswer(
            *parsed.request,
            it == open_dialogs_.end() ? nullptr : &it->second.request);
        client.out += answered.reply;
        if (answered.answer) {
          done = std::move(it->second.done);
          answer = std::move(answered.answer);
          dialog_lines_.push_back(
              telemetry::FinishNow(DialogClosedEvent(it->first, "client")));
          open_dialogs_.erase(it);
        }
      }
      if (done && answer) {
        done(std::move(*answer));
      }
      break;
    }
    case Action::kPad:
      // Decide set the command.
      if (decision.pad) {
        client.out += PadReply(parsed.request->id, pads_->Apply(*decision.pad));
      }
      break;
    case Action::kStop:
      stopping_ = true;
      controller_answers_ = false;
      for (const auto& other : clients_) {
        if (other->session.role != Role::kNone) {
          other->out += ExitEvent(EXIT_SUCCESS);
        }
      }
      break;
  }
}

void AutomationChannel::ForwardTelemetry() {
  std::vector<std::string> lines;
  {
    std::lock_guard<std::mutex> lock(telemetry_->mutex);
    lines.swap(telemetry_->lines);
  }
  if (lines.empty()) {
    return;
  }
  for (const auto& client : clients_) {
    if (!client->session.telemetry || client->closing) {
      continue;
    }
    for (const std::string& line : lines) {
      client->out += line;
      client->out += '\n';
    }
    if (client->out.size() > kMaxClientBacklog) {
      XELOGW("Automation: dropping a client that fell behind");
      client->out.clear();
      client->closing = true;
    }
  }
}

void AutomationChannel::Answer(DialogRequest request,
                               std::function<void(DialogAnswer)> done) {
  DialogAnswer default_answer = PolicyAnswer(request);
  std::unique_lock<std::mutex> lock(dialogs_mutex_);
  // Checked under the lock: CloseAllDialogs clears it first, then empties the
  // open dialogs under the lock, so none is left behind.
  if (!controller_answers_.load(std::memory_order_acquire)) {
    lock.unlock();
    done(std::move(default_answer));
    return;
  }
  uint64_t id = next_dialog_++;
  std::string event = telemetry::FinishNow(DialogEvent(
      id, request, default_answer, uint32_t(dialog_timeout_.count())));
  dialog_lines_.push_back(event);
  open_dialogs_.emplace(
      id,
      OpenDialog{std::move(request), std::move(default_answer), std::move(done),
                 std::chrono::steady_clock::now() + dialog_timeout_,
                 std::move(event)});
}

void AutomationChannel::ServiceDialogs() {
  bool answers = false;
  for (const auto& client : clients_) {
    if (client->session.role == Role::kController && client->session.dialogs &&
        !client->closing) {
      answers = true;
    }
  }
  controller_answers_.store(answers && !stopping_, std::memory_order_release);
  std::vector<std::string> lines;
  std::vector<std::pair<std::function<void(DialogAnswer)>, DialogAnswer>>
      to_answer;
  {
    std::lock_guard<std::mutex> lock(dialogs_mutex_);
    lines.swap(dialog_lines_);
    auto now = std::chrono::steady_clock::now();
    for (auto it = open_dialogs_.begin(); it != open_dialogs_.end();) {
      if (answers && now < it->second.deadline) {
        ++it;
        continue;
      }
      lines.push_back(telemetry::FinishNow(
          DialogClosedEvent(it->first, answers ? "timeout" : "policy")));
      to_answer.emplace_back(std::move(it->second.done),
                             std::move(it->second.default_answer));
      it = open_dialogs_.erase(it);
    }
  }
  for (auto& [done, answer] : to_answer) {
    done(std::move(answer));
  }
  if (lines.empty()) {
    return;
  }
  for (const auto& client : clients_) {
    if (!client->session.dialogs || client->closing) {
      continue;
    }
    for (const std::string& line : lines) {
      client->out += line;
      client->out += '\n';
    }
  }
}

void AutomationChannel::CloseAllDialogs() {
  std::map<uint64_t, OpenDialog> closing;
  {
    std::lock_guard<std::mutex> lock(dialogs_mutex_);
    closing.swap(open_dialogs_);
  }
  for (auto& [id, dialog] : closing) {
    dialog.done(std::move(dialog.default_answer));
  }
}

void AutomationChannel::ForwardPadEvents() {
  std::vector<hid::remote::PadEvent> events = pads_->Poll();
  if (events.empty()) {
    return;
  }
  std::vector<std::string> lines;
  for (const hid::remote::PadEvent& pad_event : events) {
    lines.push_back(telemetry::FinishNow(PadEvent(pad_event)));
  }
  for (const auto& client : clients_) {
    if (!client->session.input || client->closing) {
      continue;
    }
    for (const std::string& line : lines) {
      client->out += line;
      client->out += '\n';
    }
  }
}

void AutomationChannel::Stop() {
  XELOGI("Automation: stopping at a client's request");
  auto deadline = std::chrono::steady_clock::now() + kStopFlushTime;
  bool pending = true;
  while (pending && std::chrono::steady_clock::now() < deadline) {
    pending = false;
    for (const auto& client : clients_) {
      if (!client->out.empty() && Flush(*client) && !client->out.empty()) {
        pending = true;
      }
    }
    if (pending) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  for (const auto& client : clients_) {
    CloseSocket(client->socket);
  }
  clients_.clear();
  running_ = false;
  CloseAllDialogs();
  auto stop = std::move(stop_);
  stop();
}

}  // namespace automation
}  // namespace xe
