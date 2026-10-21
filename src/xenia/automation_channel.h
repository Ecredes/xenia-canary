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
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "xenia/automation_protocol.h"
#include "xenia/base/platform.h"

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
class AutomationChannel {
 public:
  // Null when --automation_port is 0 or the port can't be opened. stop ends
  // the process; it's called once, on the channel's thread, after the clients
  // have been told.
  static std::unique_ptr<AutomationChannel> Create(Emulator& emulator,
                                                   BuildInfo build,
                                                   std::function<void()> stop);
  ~AutomationChannel();

 private:
  struct Client {
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

  AutomationChannel(SocketHandle listen_socket, std::string token,
                    BuildInfo build, std::shared_ptr<SnapshotSource> snapshot,
                    std::function<void()> stop);

  void ServerLoop();
  void Accept();
  // False when the client is gone.
  bool Read(Client& client);
  bool Flush(Client& client);
  void Handle(Client& client, std::string_view line);
  void Stop();

  SocketHandle listen_socket_;
  std::string token_;
  BuildInfo build_;
  std::shared_ptr<SnapshotSource> snapshot_;
  std::function<void()> stop_;
  std::vector<std::unique_ptr<Client>> clients_;
  bool stopping_ = false;
  std::atomic<bool> running_{true};
  std::thread thread_;
};

}  // namespace automation
}  // namespace xe

#endif  // XENIA_AUTOMATION_CHANNEL_H_
