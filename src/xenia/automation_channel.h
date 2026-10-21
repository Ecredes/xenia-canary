/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_AUTOMATION_CHANNEL_H_
#define XENIA_AUTOMATION_CHANNEL_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "xenia/answer_source.h"
#include "xenia/automation_protocol.h"
#include "xenia/base/platform.h"
#include "xenia/hid/remote/remote_pads.h"

namespace xe {
class Emulator;
}  // namespace xe

namespace xe {
namespace automation {

#if XE_PLATFORM_WIN32
using SocketHandle = uintptr_t;
#else
using SocketHandle = int;
#endif

// Lets a tool watch and control this process over a loopback TCP socket
// (--automation_port), speaking automation_protocol.h. Carries out the
// protocol's decisions; decides nothing itself.
//
// As an answer source, asks the controller when one is subscribed to dialogs,
// and otherwise, at the timeout or when the controller goes, the policies.
class AutomationChannel : public AnswerSource {
 public:
  // Null when --automation_port is 0 or the port can't be opened. stop ends
  // the process; it's called once, on the channel's thread, after the clients
  // have been told.
  static std::unique_ptr<AutomationChannel> Create(Emulator& emulator,
                                                   BuildInfo build,
                                                   std::function<void()> stop);
  ~AutomationChannel() override;

  // From any thread.
  void Answer(DialogRequest request,
              std::function<void(DialogAnswer)> done) override;

  // The pads the controller drives, for the app's remote input driver.
  std::shared_ptr<hid::remote::RemotePads> pads() const { return pads_; }

 private:
  struct Client {
    // Names the client to frames captured for it, which may outlive it.
    uint64_t id;
    SocketHandle socket;
    LineSplitter splitter;
    Session session;
    std::string out;
    bool closing = false;
  };
  // What the emulator's launch and terminate events last said. Shared with
  // their listeners, which may outlive the channel.
  struct SnapshotSource {
    std::mutex mutex;
    Snapshot snapshot;
  };
  // Telemetry lines from its writer thread, waiting for the channel's.
  // Shared with the telemetry sink, which telemetry owns.
  struct TelemetryFeed {
    std::mutex mutex;
    std::string session_start;
    std::vector<std::string> lines;
  };
  class TelemetrySink;
  struct OpenDialog {
    DialogRequest request;
    DialogAnswer default_answer;
    std::function<void(DialogAnswer)> done;
    std::chrono::steady_clock::time_point deadline;
    // Its dialog event, for clients subscribing while it is open.
    std::string event;
  };
  class FrameWorker;
  // Who gets one encoding of a frame being captured: the reply to a frame
  // request (re), or a watch's frame event.
  struct FrameRecipient {
    uint64_t client;
    bool watch;
    std::string re;
    FrameOptions options;
  };

  AutomationChannel(Emulator& emulator, SocketHandle listen_socket,
                    std::string token, BuildInfo build,
                    std::shared_ptr<SnapshotSource> snapshot,
                    std::shared_ptr<TelemetryFeed> telemetry,
                    std::shared_ptr<hid::remote::RemotePads> pads,
                    std::unique_ptr<FrameWorker> frames,
                    std::function<void()> stop);

  void ServerLoop();
  void Accept();
  // False when the client is gone.
  bool Read(Client& client);
  bool Flush(Client& client);
  void Handle(Client& client, std::string_view line);
  // Hands queued telemetry to its subscribers, and frame_stats to benches.
  void ForwardTelemetry();
  // Sends dialog events, and answers dialogs past their deadline, or all of
  // them when no controller is subscribed any more.
  void ServiceDialogs();
  // Answers every open dialog with the policies.
  void CloseAllDialogs();
  // Sends pad events to their subscribers.
  void ForwardPadEvents();
  // Delivers the captured frame, then starts capturing the next if a request
  // waits or a watch is due.
  void ServiceFrames();
  // A watch or a capture needs the loop to come round sooner.
  bool FramesBusy() const;
  void Stop();

  SocketHandle listen_socket_;
  std::string token_;
  BuildInfo build_;
  std::shared_ptr<SnapshotSource> snapshot_;
  std::shared_ptr<TelemetryFeed> telemetry_;
  std::shared_ptr<hid::remote::RemotePads> pads_;
  Emulator& emulator_;
  std::unique_ptr<FrameWorker> frames_;
  std::function<void()> stop_;
  std::vector<std::unique_ptr<Client>> clients_;
  uint64_t next_client_ = 1;
  // Frame requests waiting for the next capture, and the recipients of the
  // one in progress.
  std::vector<FrameRecipient> frame_requests_;
  std::vector<FrameRecipient> frame_recipients_;
  // The capture's encodings, as the worker returns them.
  std::vector<FrameOptions> frame_encodings_;
  bool stopping_ = false;
  std::atomic<bool> running_{true};

  std::chrono::milliseconds dialog_timeout_;
  // A controller is subscribed to dialogs. Set by the channel's thread.
  std::atomic<bool> controller_answers_{false};
  std::mutex dialogs_mutex_;
  std::map<uint64_t, OpenDialog> open_dialogs_;
  uint64_t next_dialog_ = 1;
  // Dialog lines for the channel's thread to send.
  std::vector<std::string> dialog_lines_;
  std::thread thread_;
};

}  // namespace automation
}  // namespace xe

#endif  // XENIA_AUTOMATION_CHANNEL_H_
