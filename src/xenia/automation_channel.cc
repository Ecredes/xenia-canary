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
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <random>
#include <utility>

#include "xenia/apu/audio_system.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/telemetry.h"
#include "xenia/base/threading.h"
#include "xenia/emulator.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/ui/presenter.h"

#include "third_party/stb/stb_image_write.h"

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
// The round time while frames are watched or captured, so they leave soon
// after they are due or ready.
constexpr int32_t kFramePollMicroseconds = 2000;
// A watcher with this much unsent skips frames rather than queueing them.
constexpr size_t kMaxFrameBacklog = 1024 * 1024;
// The most a video stream asks the OS to buffer.
constexpr size_t kMaxVideoSendBuffer = 64 * 1024 * 1024;
// The longest a video stream sleeps between looks at the clock.
constexpr uint64_t kVideoSleepNs = 5000000;
// A video listener reading nothing for this long ends the stream.
constexpr auto kVideoStallTime = std::chrono::seconds(10);
// Audio a client has stopped submitting is sent after this long.
constexpr uint64_t kAudioFlushNs = 200000000;
// Audio chunks kept for the channel's thread between its rounds (about 10 s).
constexpr size_t kMaxQueuedAudioChunks = 100;

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

// Captures the guest output and encodes it on its own thread, so neither the
// GPU's thread nor the channel's waits for the readback. One capture at a
// time, encoded once per distinct FrameOptions.
class AutomationChannel::FrameWorker {
 public:
  // Fills the image and the guest's counters just before the capture; false
  // when there is no guest output.
  using Capture = std::function<bool(ui::RawImage& image, uint64_t& vblank,
                                     uint64_t& swap)>;
  // One per requested encoding, empty when there was nothing to capture.
  using Result = std::vector<std::optional<EncodedFrame>>;

  explicit FrameWorker(Capture capture) : capture_(std::move(capture)) {
    thread_ = std::thread([this]() { Loop(); });
  }
  ~FrameWorker() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      running_ = false;
    }
    wake_.notify_one();
    thread_.join();
  }

  // Captured and not yet taken, or capturing.
  bool busy() {
    std::lock_guard<std::mutex> lock(mutex_);
    return job_ || result_;
  }
  // Only when not busy.
  void Submit(std::vector<FrameOptions> encodings) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      job_ = std::move(encodings);
    }
    wake_.notify_one();
  }
  std::optional<Result> TakeResult() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::optional<Result> result = std::move(result_);
    if (result) {
      result_.reset();
      job_.reset();
    }
    return result;
  }

 private:
  void Loop() {
    xe::threading::set_name("Automation Frames");
    while (true) {
      std::vector<FrameOptions> encodings;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait(lock, [this]() { return !running_ || (job_ && !result_); });
        if (!running_ || !job_) {
          return;
        }
        encodings = *job_;
      }
      Result result = Run(encodings);
      std::lock_guard<std::mutex> lock(mutex_);
      result_ = std::move(result);
    }
  }

  Result Run(const std::vector<FrameOptions>& encodings) {
    Result result(encodings.size());
    ui::RawImage image;
    EncodedFrame captured;
    captured.ns = telemetry::NowNs();
    captured.state = telemetry::CurrentState();
    if (!capture_(image, captured.vblank, captured.swap)) {
      return result;
    }
    captured.capture_ns = telemetry::NowNs() - captured.ns;
    for (size_t i = 0; i < encodings.size(); ++i) {
      const FrameOptions& options = encodings[i];
      uint64_t scale_start = telemetry::NowNs();
      RgbImage rgb = ScaleToWidth(image.data.data(), image.width, image.height,
                                  image.stride, options.max_width);
      uint64_t encode_start = telemetry::NowNs();
      EncodedFrame frame = captured;
      frame.scale_ns = encode_start - scale_start;
      frame.format = options.format;
      frame.width = rgb.width;
      frame.height = rgb.height;
      stbi_write_func* append = [](void* context, void* data, int size) {
        auto* bytes = static_cast<std::vector<uint8_t>*>(context);
        auto* begin = static_cast<const uint8_t*>(data);
        bytes->insert(bytes->end(), begin, begin + size);
      };
      int written =
          options.format == FrameFormat::kPng
              ? stbi_write_png_to_func(append, &frame.bytes, int(rgb.width),
                                       int(rgb.height), 3, rgb.pixels.data(),
                                       int(rgb.width * 3))
              : stbi_write_jpg_to_func(append, &frame.bytes, int(rgb.width),
                                       int(rgb.height), 3, rgb.pixels.data(),
                                       int(options.quality));
      frame.encode_ns = telemetry::NowNs() - encode_start;
      if (written && rgb.width) {
        result[i] = std::move(frame);
      }
    }
    return result;
  }

  Capture capture_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool running_ = true;
  std::optional<std::vector<FrameOptions>> job_;
  std::optional<Result> result_;
  std::thread thread_;
};

// Sends a video_start's frames on its own thread, so neither the channel's
// thread nor the GPU's waits for the listener.
class AutomationChannel::VideoStream {
 public:
  using Capture = FrameWorker::Capture;

  // Null, with error set, when nothing listens on the port.
  static std::unique_ptr<VideoStream> Connect(
      const VideoOptions& options, Capture capture,
      std::function<uint64_t()> swap_count, std::string* error) {
    SocketHandle socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == kInvalidSocket) {
      *error = "unable to open a socket";
      return nullptr;
    }
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(options.port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(socket_handle, reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) != 0 ||
        !SetNonBlocking(socket_handle)) {
      CloseSocket(socket_handle);
      *error = "nothing listens on 127.0.0.1:" + std::to_string(options.port);
      return nullptr;
    }
    // A few frames of slack, so one slow read doesn't cost a tick.
    int buffer = int(std::min(size_t(options.width) * options.height * 3 * 4,
                              kMaxVideoSendBuffer));
    setsockopt(socket_handle, SOL_SOCKET, SO_SNDBUF,
               reinterpret_cast<const char*>(&buffer), sizeof(buffer));
    return std::unique_ptr<VideoStream>(new VideoStream(
        options, socket_handle, std::move(capture), std::move(swap_count)));
  }
  ~VideoStream() {
    Stop();
    CloseSocket(socket_);
  }

  // Ends the stream; counts are final after.
  void Stop() {
    running_ = false;
    if (thread_.joinable()) {
      thread_.join();
    }
  }
  // video_frame lines for the client that started it.
  std::vector<std::string> TakeLines() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> lines;
    lines.swap(lines_);
    return lines;
  }
  // Why it ended by itself, empty while it runs.
  std::string end_reason() {
    std::lock_guard<std::mutex> lock(mutex_);
    return end_reason_;
  }
  VideoCounts counts() {
    std::lock_guard<std::mutex> lock(mutex_);
    return counts_;
  }
  // When frame 0 is due, in host ns.
  uint64_t start_ns() const { return start_ns_; }

 private:
  VideoStream(const VideoOptions& options, SocketHandle socket_handle,
              Capture capture, std::function<uint64_t()> swap_count)
      : options_(options),
        socket_(socket_handle),
        capture_(std::move(capture)),
        swap_count_(std::move(swap_count)),
        start_ns_(telemetry::NowNs()) {
    thread_ = std::thread([this]() { Loop(); });
  }

  void Loop() {
    xe::threading::set_name("Automation Video");
    VideoClock clock;
    clock.fps = options_.fps;
    clock.start_ns = start_ns_;
    // Black until the guest has output.
    RgbImage last = FitInto(nullptr, 0, 0, 0, options_.width, options_.height);
    while (running_) {
      uint64_t now_ns = telemetry::NowNs();
      uint64_t next_ns = VideoTickNs(clock, clock.ticks);
      if (now_ns < next_ns) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(
            std::min(next_ns - now_ns, kVideoSleepNs)));
        continue;
      }
      VideoStep step =
          NextVideoStep(clock, now_ns, swap_count_(), options_.fps / 2);
      for (uint64_t i = 0; i < step.repeats; ++i) {
        if (!Send(last.pixels)) {
          return;
        }
      }
      std::string line;
      if (step.capture) {
        ui::RawImage image;
        EncodedFrame frame;
        frame.ns = telemetry::NowNs();
        frame.state = telemetry::CurrentState();
        if (capture_(image, frame.vblank, frame.swap)) {
          frame.capture_ns = telemetry::NowNs() - frame.ns;
          last = FitInto(image.data.data(), image.width, image.height,
                         image.stride, options_.width, options_.height);
          line = VideoFrameLine(clock.ticks - 1, frame);
        }
      }
      if (!Send(last.pixels)) {
        return;
      }
      std::lock_guard<std::mutex> lock(mutex_);
      counts_.frames += step.repeats + 1;
      counts_.dropped += step.dropped;
      if (!line.empty()) {
        ++counts_.captures;
        lines_.push_back(std::move(line));
      }
    }
  }

  // False when the stream is stopping or has ended.
  bool Send(const std::vector<uint8_t>& bytes) {
    size_t sent = 0;
    auto progress = std::chrono::steady_clock::now();
    while (sent < bytes.size()) {
      if (!running_) {
        return false;
      }
      int count =
          SendSome(socket_, reinterpret_cast<const char*>(bytes.data()) + sent,
                   bytes.size() - sent);
      if (count > 0) {
        sent += size_t(count);
        progress = std::chrono::steady_clock::now();
        continue;
      }
      if (count < 0 && WouldBlock()) {
        if (std::chrono::steady_clock::now() - progress > kVideoStallTime) {
          End("the listener stopped reading");
          return false;
        }
        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(socket_, &write_set);
        timeval timeout = {0, kPollMicroseconds};
        select(int(socket_ + 1), nullptr, &write_set, nullptr, &timeout);
        continue;
      }
      End("the listener closed the connection");
      return false;
    }
    return true;
  }

  void End(std::string reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    end_reason_ = std::move(reason);
  }

  VideoOptions options_;
  SocketHandle socket_;
  Capture capture_;
  std::function<uint64_t()> swap_count_;
  uint64_t start_ns_;
  std::atomic<bool> running_{true};
  std::mutex mutex_;
  std::vector<std::string> lines_;
  std::string end_reason_;
  VideoCounts counts_;
  std::thread thread_;
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

  // Frames and video capture one at a time, so their readbacks of the guest
  // output don't interleave.
  auto capture_mutex = std::make_shared<std::mutex>();
  FrameWorker::Capture capture = [&emulator, capture_mutex](ui::RawImage& image,
                                                            uint64_t& vblank,
                                                            uint64_t& swap) {
    gpu::GraphicsSystem* graphics_system = emulator.graphics_system();
    ui::Presenter* presenter =
        graphics_system ? graphics_system->presenter() : nullptr;
    if (!presenter) {
      return false;
    }
    std::lock_guard<std::mutex> lock(*capture_mutex);
    vblank = graphics_system->vblank_count();
    swap = graphics_system->swap_count();
    return presenter->CaptureGuestOutput(image);
  };
  auto frames = std::make_unique<FrameWorker>(capture);
  VideoStarter start_video = [&emulator, capture](const VideoOptions& options,
                                                  std::string* error) {
    return VideoStream::Connect(
        options, capture,
        [&emulator]() {
          gpu::GraphicsSystem* graphics_system = emulator.graphics_system();
          return graphics_system ? graphics_system->swap_count() : 0;
        },
        error);
  };

  XELOGI("Automation: listening on 127.0.0.1:{}", port);
  return std::unique_ptr<AutomationChannel>(new AutomationChannel(
      emulator, listen_socket, std::move(token), std::move(build),
      std::move(snapshot), std::move(telemetry_feed), std::move(pads),
      std::move(frames), std::move(start_video), std::move(stop)));
}

AutomationChannel::AutomationChannel(
    Emulator& emulator, SocketHandle listen_socket, std::string token,
    BuildInfo build, std::shared_ptr<SnapshotSource> snapshot,
    std::shared_ptr<TelemetryFeed> telemetry,
    std::shared_ptr<hid::remote::RemotePads> pads,
    std::unique_ptr<FrameWorker> frames, VideoStarter start_video,
    std::function<void()> stop)
    : listen_socket_(listen_socket),
      token_(std::move(token)),
      build_(std::move(build)),
      snapshot_(std::move(snapshot)),
      telemetry_(std::move(telemetry)),
      pads_(std::move(pads)),
      emulator_(emulator),
      frames_(std::move(frames)),
      start_video_(std::move(start_video)),
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
  video_.reset();
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
    // An advance pauses as soon as its last vblank has passed.
    timeval timeout = {0, FramesBusy() || advance_ ? kFramePollMicroseconds
                                                   : kPollMicroseconds};
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
    ServiceFrames();
    ServiceVideo();
    ServiceAudio();
    ServiceAdvance();
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
          // Nor can anyone resume it: a run left paused would never go on.
          ReleasePause();
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
    client->id = next_client_++;
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
    case Action::kFrame:
      frame_requests_.push_back(
          {client.id, false, parsed.request->id, decision.frame_options});
      break;
    case Action::kVideoStart: {
      if (video_) {
        client.out +=
            ErrorLine(parsed.request->id, "video is already streaming");
        break;
      }
      std::string error;
      video_ = start_video_(decision.video_options, &error);
      if (!video_) {
        client.out += ErrorLine(parsed.request->id, error);
        break;
      }
      video_client_ = client.id;
      client.out += VideoStartReply(parsed.request->id, decision.video_options,
                                    video_->start_ns());
      break;
    }
    case Action::kVideoStop:
      if (!video_ || video_client_ != client.id) {
        client.out += ErrorLine(parsed.request->id, "no video streaming");
        break;
      }
      video_->Stop();
      for (const std::string& line : video_->TakeLines()) {
        client.out += line;
      }
      client.out += VideoStopReply(parsed.request->id, video_->counts());
      video_.reset();
      break;
    case Action::kPause:
    case Action::kResume:
    case Action::kAdvance: {
      gpu::GraphicsSystem* graphics_system = emulator_.graphics_system();
      if (!graphics_system) {
        client.out += ErrorLine(parsed.request->id, "no title running");
        break;
      }
      if (advance_) {
        client.out += ErrorLine(parsed.request->id, "an advance is under way");
        break;
      }
      if (decision.action == Action::kAdvance) {
        if (!emulator_.is_paused()) {
          client.out += ErrorLine(parsed.request->id, "not paused");
          break;
        }
        // The vblank thread delivers exactly this many, then holds.
        uint64_t target = graphics_system->vblank_count() + decision.vblanks;
        graphics_system->set_vblank_limit(target);
        advance_ = Advance{client.id, parsed.request->id, target};
        emulator_.Resume();
        break;
      }
      if (decision.action == Action::kPause) {
        emulator_.Pause();
        paused_by_client_ = true;
      } else {
        emulator_.Resume();
        paused_by_client_ = false;
      }
      client.out += PauseReply(parsed.request->id, emulator_.is_paused(),
                               graphics_system->vblank_count(),
                               graphics_system->swap_count());
      break;
    }
    case Action::kStop:
      // Suspended guest threads would hold the shutdown up.
      ReleasePause();
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
    if (client->session.bench) {
      for (const std::string& line : lines) {
        AddFrameStats(*client->session.bench, line);
      }
    }
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

bool AutomationChannel::FramesBusy() const {
  if (!frame_requests_.empty() || !frame_recipients_.empty()) {
    return true;
  }
  return std::any_of(clients_.begin(), clients_.end(), [](const auto& client) {
    return client->session.frames.has_value();
  });
}

void AutomationChannel::ServiceFrames() {
  if (std::optional<FrameWorker::Result> result = frames_->TakeResult()) {
    for (const FrameRecipient& recipient : frame_recipients_) {
      auto client = std::find_if(
          clients_.begin(), clients_.end(),
          [&recipient](const auto& c) { return c->id == recipient.client; });
      if (client == clients_.end() || (*client)->closing) {
        continue;
      }
      size_t encoding =
          size_t(std::find(frame_encodings_.begin(), frame_encodings_.end(),
                           recipient.options) -
                 frame_encodings_.begin());
      const std::optional<EncodedFrame>& frame = (*result)[encoding];
      if (recipient.watch) {
        if (frame) {
          (*client)->out += FrameEventLine(*frame);
        }
      } else {
        (*client)->out += frame ? FrameReply(recipient.re, *frame)
                                : ErrorLine(recipient.re, "no frame yet");
      }
    }
    frame_recipients_.clear();
  }
  if (frames_->busy()) {
    return;
  }
  std::vector<FrameRecipient> recipients;
  recipients.swap(frame_requests_);
  uint64_t now_ns = telemetry::NowNs();
  gpu::GraphicsSystem* graphics_system = emulator_.graphics_system();
  uint64_t swap = graphics_system ? graphics_system->swap_count() : 0;
  for (const auto& client : clients_) {
    std::optional<FrameWatch>& watch = client->session.frames;
    if (!watch || client->closing || !FrameDue(*watch, now_ns, swap)) {
      continue;
    }
    // A client still sending the last one gets a later frame instead.
    if (client->out.size() < kMaxFrameBacklog) {
      recipients.push_back({client->id, true, "", watch->options});
    }
    AdvanceWatch(*watch, now_ns, swap);
  }
  if (recipients.empty()) {
    return;
  }
  frame_encodings_.clear();
  for (const FrameRecipient& recipient : recipients) {
    if (std::find(frame_encodings_.begin(), frame_encodings_.end(),
                  recipient.options) == frame_encodings_.end()) {
      frame_encodings_.push_back(recipient.options);
    }
  }
  frame_recipients_ = std::move(recipients);
  frames_->Submit(frame_encodings_);
}

void AutomationChannel::ServiceAudio() {
  bool wanted =
      std::any_of(clients_.begin(), clients_.end(), [](const auto& client) {
        return client->session.audio && !client->closing;
      });
  audio_->active = wanted;
  if (wanted && !audio_tap_installed_) {
    if (apu::AudioSystem* audio_system = emulator_.audio_system()) {
      audio_system->SetFrameTap([feed = audio_, &emulator = emulator_](
                                    size_t index, const float* samples) {
        if (!feed->active.load(std::memory_order_relaxed)) {
          return;
        }
        std::array<int16_t, kGuestAudioFrameSamples * 2> stereo;
        DownmixGuestFrame(samples, stereo.data());
        uint64_t vblank = 0, swap = 0;
        if (gpu::GraphicsSystem* graphics_system = emulator.graphics_system()) {
          vblank = graphics_system->vblank_count();
          swap = graphics_system->swap_count();
        }
        std::lock_guard<std::mutex> lock(feed->mutex);
        feed->chunker.Add(uint32_t(index), telemetry::NowNs(), vblank, swap,
                          stereo.data(), kGuestAudioFrameSamples, feed->chunks);
        if (feed->chunks.size() > kMaxQueuedAudioChunks) {
          feed->chunks.erase(feed->chunks.begin());
        }
      });
      audio_tap_installed_ = true;
    }
  }
  std::vector<AudioChunk> chunks;
  {
    std::lock_guard<std::mutex> lock(audio_->mutex);
    if (!wanted) {
      audio_->chunker.Clear();
      audio_->chunks.clear();
      return;
    }
    audio_->chunker.Flush(telemetry::NowNs(), kAudioFlushNs, audio_->chunks);
    chunks.swap(audio_->chunks);
  }
  telemetry::State state = telemetry::CurrentState();
  for (const AudioChunk& chunk : chunks) {
    std::string line = AudioEventLine(chunk, state);
    for (const auto& client : clients_) {
      if (client->session.audio && !client->closing) {
        client->out += line;
      }
    }
  }
}

void AutomationChannel::ServiceVideo() {
  if (!video_) {
    return;
  }
  auto owner = std::find_if(
      clients_.begin(), clients_.end(),
      [this](const auto& client) { return client->id == video_client_; });
  if (owner == clients_.end() || (*owner)->closing) {
    video_.reset();
    return;
  }
  for (const std::string& line : video_->TakeLines()) {
    (*owner)->out += line;
  }
  std::string reason = video_->end_reason();
  if (!reason.empty()) {
    video_->Stop();
    (*owner)->out +=
        telemetry::FinishNow(VideoEndEvent(reason, video_->counts())) + "\n";
    video_.reset();
  }
}

void AutomationChannel::ServiceAdvance() {
  if (!advance_) {
    return;
  }
  gpu::GraphicsSystem* graphics_system = emulator_.graphics_system();
  if (!graphics_system || graphics_system->vblank_count() < advance_->target) {
    return;
  }
  // No vblank comes past the limit, so the guest is still in the last one.
  emulator_.Pause();
  graphics_system->set_vblank_limit(0);
  paused_by_client_ = true;
  auto client =
      std::find_if(clients_.begin(), clients_.end(),
                   [this](const auto& c) { return c->id == advance_->client; });
  if (client != clients_.end() && !(*client)->closing) {
    (*client)->out +=
        PauseReply(advance_->re, true, graphics_system->vblank_count(),
                   graphics_system->swap_count());
  }
  advance_.reset();
}

void AutomationChannel::ReleasePause() {
  gpu::GraphicsSystem* graphics_system = emulator_.graphics_system();
  if (advance_ && graphics_system) {
    graphics_system->set_vblank_limit(0);
  }
  if ((paused_by_client_ || advance_) && emulator_.is_paused()) {
    emulator_.Resume();
  }
  advance_.reset();
  paused_by_client_ = false;
}

void AutomationChannel::Stop() {
  XELOGI("Automation: stopping at a client's request");
  video_.reset();
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
