#pragma once

#include "App.h"
#include "host_bridge.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace mog::http {

struct RequestState;
struct ServerState;
struct WebSocketState;

struct ConnectionData {
  std::shared_ptr<WebSocketState> state;
};

using NativeWebSocket = uWS::WebSocket<false, true, ConnectionData>;

enum class WebSocketCallback { Open, Text, Binary, Drain, Close };

struct WebSocketRouteState {
  explicit WebSocketRouteState(HostBridge bridge) : host(std::move(bridge)) {}

  bool setCallback(WebSocketCallback kind, const ExprPackageValue &callback,
                   std::string &error);
  bool configurePayload(int64_t bytes, std::string &error);
  bool configureBackpressure(int64_t bytes, std::string &error);
  bool configureIdleTimeout(int64_t seconds, std::string &error);
  bool configureCloseOnLimit(bool enabled, std::string &error);
  void releaseCallbacks();

  HostBridge host;
  std::unique_ptr<PersistentRoot> onOpen;
  std::unique_ptr<PersistentRoot> onText;
  std::unique_ptr<PersistentRoot> onBinary;
  std::unique_ptr<PersistentRoot> onDrain;
  std::unique_ptr<PersistentRoot> onClose;
  uint32_t maxPayloadLength = 64 * 1024;
  uint32_t maxBackpressure = 1024 * 1024;
  uint16_t idleTimeout = 120;
  bool closeOnBackpressureLimit = true;
  bool registered = false;
};

struct WebSocketState {
  NativeWebSocket *native = nullptr;
  std::weak_ptr<ServerState> server;
  std::shared_ptr<WebSocketRouteState> route;
  std::shared_ptr<RequestState> pendingRequest;
  std::string remoteAddress;
  std::unique_ptr<PersistentRoot> socketData;
  bool open = false;
  bool closing = false;
};

bool registerWebSocket(const std::shared_ptr<ServerState> &server,
                       std::string path,
                       const std::shared_ptr<WebSocketRouteState> &route,
                       std::string &error);
bool requireOpenSocket(const std::shared_ptr<WebSocketState> &socket,
                       std::string &error);
int64_t sendWebSocket(const std::shared_ptr<WebSocketState> &socket,
                      std::string_view message, uWS::OpCode opcode,
                      std::string &error);

} // namespace mog::http
