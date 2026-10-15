/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/remote/remote_input_driver.h"

#include <chrono>
#include <utility>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/logging.h"

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
#include <unistd.h>
#include <cerrno>
#endif

namespace xe {
namespace hid {
namespace remote {

namespace {

#if XE_PLATFORM_WIN32
constexpr SocketHandle kInvalidSocket = SocketHandle(INVALID_SOCKET);
void CloseSocket(SocketHandle socket) { closesocket(SOCKET(socket)); }
bool WouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
bool SetNonBlocking(SocketHandle socket) {
  u_long enabled = 1;
  return ioctlsocket(SOCKET(socket), FIONBIO, &enabled) == 0;
}
#else
constexpr SocketHandle kInvalidSocket = -1;
void CloseSocket(SocketHandle socket) { close(socket); }
bool WouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }
bool SetNonBlocking(SocketHandle socket) {
  int flags = fcntl(socket, F_GETFL, 0);
  return flags != -1 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}
#endif

constexpr size_t kReadChunk = 4096;
// Select timeout, which bounds how late begin/end and frame lines are sent.
constexpr long kPollMicroseconds = 1000;

// Same clock as std::chrono::steady_clock in tools (QueryPerformanceCounter on
// Windows, CLOCK_MONOTONIC elsewhere), so their timestamps line up.
uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

}  // namespace

RemoteInputDriver::RemoteInputDriver(xe::ui::Window* window,
                                     size_t window_z_order, uint16_t port,
                                     FrameSource frame_source)
    : InputDriver(window, window_z_order),
      port_(port),
      frame_source_(std::move(frame_source)),
      listen_socket_(kInvalidSocket),
      client_socket_(kInvalidSocket) {}

RemoteInputDriver::~RemoteInputDriver() {
  running_ = false;
  if (thread_.joinable()) {
    thread_.join();
  }
  if (client_socket_ != kInvalidSocket) {
    CloseSocket(client_socket_);
  }
  if (listen_socket_ != kInvalidSocket) {
    CloseSocket(listen_socket_);
  }
#if XE_PLATFORM_WIN32
  if (winsock_started_) {
    WSACleanup();
  }
#endif
}

X_STATUS RemoteInputDriver::Setup() {
#if XE_PLATFORM_WIN32
  WSADATA wsa_data;
  if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
    XELOGE("Remote input: WSAStartup failed");
    return X_STATUS_UNSUCCESSFUL;
  }
  winsock_started_ = true;
#endif
  SocketHandle listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listen_socket == kInvalidSocket) {
    XELOGE("Remote input: unable to create a socket");
    return X_STATUS_UNSUCCESSFUL;
  }
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_port = htons(port_);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(listen_socket, reinterpret_cast<sockaddr*>(&address),
           sizeof(address)) != 0 ||
      listen(listen_socket, 1) != 0 || !SetNonBlocking(listen_socket)) {
    XELOGE("Remote input: unable to listen on 127.0.0.1:{}", port_);
    CloseSocket(listen_socket);
    return X_STATUS_UNSUCCESSFUL;
  }
  listen_socket_ = listen_socket;
  running_ = true;
  thread_ = std::thread([this]() { ServerLoop(); });
  XELOGI("Remote input: listening on 127.0.0.1:{}", port_);
  return X_STATUS_SUCCESS;
}

const PadState& RemoteInputDriver::CurrentState(uint32_t user_index,
                                                uint64_t vblank,
                                                bool guest_read) {
  User& user = users_[user_index];
  const PadState& state =
      user.queue.Advance(vblank, user_index, &pending_events_);
  const uint32_t active_id = user.queue.active_id();
  if (guest_read && active_id != 0 && active_id != user.seen_id) {
    user.seen_id = active_id;
    pending_events_.push_back(
        {PadEvent::Kind::kSeen, user_index, active_id, vblank});
  }
  if (state != user.last_state) {
    user.last_state = state;
    ++user.packet_number;
  }
  return state;
}

void RemoteInputDriver::ResetUsers() {
  for (User& user : users_) {
    user.connected = false;
    user.queue.Clear();
    user.queue.set_base(PadState());
  }
}

X_RESULT RemoteInputDriver::GetCapabilities(uint32_t user_index, uint32_t flags,
                                            X_INPUT_CAPABILITIES* out_caps) {
  if (user_index >= kUserCount || !out_caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!users_[user_index].connected) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  out_caps->type = X_INPUT_DEVTYPE::XINPUT_DEVTYPE_GAMEPAD;
  out_caps->sub_type = X_INPUT_DEVSUBTYPE::XINPUT_DEVSUBTYPE_GAMEPAD;
  out_caps->flags = 0;
  out_caps->gamepad.buttons = 0xFFFF;
  out_caps->gamepad.left_trigger = 0xFF;
  out_caps->gamepad.right_trigger = 0xFF;
  out_caps->gamepad.thumb_lx = int16_t(-1);
  out_caps->gamepad.thumb_ly = int16_t(-1);
  out_caps->gamepad.thumb_rx = int16_t(-1);
  out_caps->gamepad.thumb_ry = int16_t(-1);
  out_caps->vibration.left_motor_speed = 0xFFFF;
  out_caps->vibration.right_motor_speed = 0xFFFF;
  return X_ERROR_SUCCESS;
}

X_RESULT RemoteInputDriver::GetState(uint32_t user_index,
                                     X_INPUT_STATE* out_state) {
  if (user_index >= kUserCount || !out_state) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  const uint64_t vblank = frame_source_().vblank;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!users_[user_index].connected) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  const PadState& state = CurrentState(user_index, vblank, true);
  out_state->packet_number = users_[user_index].packet_number;
  out_state->gamepad.buttons = state.buttons;
  out_state->gamepad.left_trigger = state.left_trigger;
  out_state->gamepad.right_trigger = state.right_trigger;
  out_state->gamepad.thumb_lx = state.thumb_lx;
  out_state->gamepad.thumb_ly = state.thumb_ly;
  out_state->gamepad.thumb_rx = state.thumb_rx;
  out_state->gamepad.thumb_ry = state.thumb_ry;
  return X_ERROR_SUCCESS;
}

X_RESULT RemoteInputDriver::SetState(uint32_t user_index,
                                     X_INPUT_VIBRATION* vibration) {
  if (user_index >= kUserCount) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  return users_[user_index].connected ? X_ERROR_SUCCESS
                                      : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT RemoteInputDriver::GetKeystroke(uint32_t users, uint32_t flags,
                                         X_INPUT_KEYSTROKE* out_keystroke) {
  const bool any_user = users == XUserIndexAny;
  if ((users >= kUserCount && !any_user) || !out_keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  const uint64_t vblank = frame_source_().vblank;
  std::lock_guard<std::mutex> lock(mutex_);
  bool any_connected = false;
  const uint32_t first = any_user ? 0 : users;
  const uint32_t last = any_user ? kUserCount : users + 1;
  for (uint32_t user_index = first; user_index < last; ++user_index) {
    User& user = users_[user_index];
    if (!user.connected) {
      continue;
    }
    any_connected = true;
    const uint32_t bits = KeystrokeBits(CurrentState(user_index, vblank, true));
    auto key = NextKeystroke(bits, &user.keystroke_bits);
    if (!key) {
      continue;
    }
    out_keystroke->virtual_key = key->virtual_key;
    out_keystroke->unicode = 0;
    out_keystroke->flags =
        key->down ? X_INPUT_KEYSTROKE_KEYDOWN : X_INPUT_KEYSTROKE_KEYUP;
    out_keystroke->user_index = uint8_t(user_index);
    out_keystroke->hid_code = 0;
    return X_ERROR_SUCCESS;
  }
  return any_connected ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

void RemoteInputDriver::ServerLoop() {
  std::vector<PadEvent> events;
  while (running_) {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(listen_socket_, &read_set);
    SocketHandle max_socket = listen_socket_;
    if (client_socket_ != kInvalidSocket) {
      FD_SET(client_socket_, &read_set);
      max_socket = std::max(max_socket, client_socket_);
    }
    timeval timeout = {0, kPollMicroseconds};
    int ready =
        select(int(max_socket + 1), &read_set, nullptr, nullptr, &timeout);
    if (ready > 0) {
      if (FD_ISSET(listen_socket_, &read_set)) {
        AcceptClient();
      }
      if (client_socket_ != kInvalidSocket &&
          FD_ISSET(client_socket_, &read_set)) {
        ReadClient();
      }
    }
    if (client_socket_ == kInvalidSocket) {
      continue;
    }

    // Report queued states starting and ending even while the guest is not
    // polling, plus any seen events from guest threads.
    const FrameCounters counters = frame_source_();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (uint32_t user_index = 0; user_index < kUserCount; ++user_index) {
        if (users_[user_index].connected) {
          CurrentState(user_index, counters.vblank, false);
        }
      }
      events.swap(pending_events_);
    }
    const uint64_t now = NowNs();
    for (const PadEvent& event : events) {
      SendLine(FormatEvent(event, now));
    }
    events.clear();
    if (watch_frames_ && counters.vblank != last_vblank_) {
      SendLine(
          fmt::format("frame {} {} {}\n", counters.vblank, counters.swap, now));
    }
    last_vblank_ = counters.vblank;
  }
}

void RemoteInputDriver::AcceptClient() {
  SocketHandle socket = accept(listen_socket_, nullptr, nullptr);
  if (socket == kInvalidSocket) {
    return;
  }
  if (client_socket_ != kInvalidSocket) {
    // One tool at a time.
    static constexpr std::string_view kBusy = "error busy\n";
    send(socket, kBusy.data(), int(kBusy.size()), 0);
    CloseSocket(socket);
    return;
  }
  if (!SetNonBlocking(socket)) {
    CloseSocket(socket);
    return;
  }
  client_socket_ = socket;
  read_buffer_.clear();
  watch_frames_ = false;
  XELOGI("Remote input: client connected");
}

void RemoteInputDriver::ReadClient() {
  char chunk[kReadChunk];
  int received = recv(client_socket_, chunk, int(sizeof(chunk)), 0);
  if (received == 0 || (received < 0 && !WouldBlock())) {
    DropClient();
    return;
  }
  if (received < 0) {
    return;
  }
  read_buffer_.append(chunk, size_t(received));
  size_t start = 0;
  // Bounded by the bytes just received.
  for (size_t end = read_buffer_.find('\n');
       end != std::string::npos && client_socket_ != kInvalidSocket;
       end = read_buffer_.find('\n', start)) {
    HandleLine(std::string_view(read_buffer_).substr(start, end - start));
    start = end + 1;
  }
  if (client_socket_ == kInvalidSocket) {
    return;
  }
  read_buffer_.erase(0, start);
  if (read_buffer_.size() > kMaxLineLength) {
    SendLine("error line too long\n");
    DropClient();
  }
}

void RemoteInputDriver::HandleLine(std::string_view line) {
  std::string error;
  auto command = ParseCommand(line, &error);
  if (!command) {
    SendLine(fmt::format("error {}\n", error));
    return;
  }
  const FrameCounters counters = frame_source_();
  std::string reply = "ok\n";
  {
    std::lock_guard<std::mutex> lock(mutex_);
    User& user = users_[command->user];
    switch (command->type) {
      case Command::Type::kHello:
        reply = fmt::format("hello xenia-remote-input {} {}\n",
                            kProtocolVersion, kUserCount);
        break;
      case Command::Type::kPing:
        reply = fmt::format("pong {} {} {}\n", counters.vblank, counters.swap,
                            NowNs());
        break;
      case Command::Type::kConnect:
        user.connected = command->value != 0;
        if (!user.connected) {
          user.queue.Clear();
          user.queue.set_base(PadState());
        }
        break;
      case Command::Type::kSet:
        user.queue.set_base(command->state);
        break;
      case Command::Type::kQueue:
        if (!user.queue.Enqueue(command->id, command->frames, command->state,
                                counters.vblank)) {
          reply = "error queue full\n";
        }
        break;
      case Command::Type::kClear:
        user.queue.Clear();
        break;
      case Command::Type::kWatch:
        watch_frames_ = command->value != 0;
        break;
    }
  }
  SendLine(reply);
}

void RemoteInputDriver::SendLine(std::string_view line) {
  if (client_socket_ == kInvalidSocket) {
    return;
  }
  int sent = send(client_socket_, line.data(), int(line.size()), 0);
  if (sent != int(line.size())) {
    // The tool stopped reading (or the send buffer is full); never block the
    // server thread on it.
    DropClient();
  }
}

void RemoteInputDriver::DropClient() {
  if (client_socket_ != kInvalidSocket) {
    CloseSocket(client_socket_);
    client_socket_ = kInvalidSocket;
    XELOGI("Remote input: client disconnected");
  }
  read_buffer_.clear();
  watch_frames_ = false;
  std::lock_guard<std::mutex> lock(mutex_);
  ResetUsers();
  pending_events_.clear();
}

}  // namespace remote
}  // namespace hid
}  // namespace xe
