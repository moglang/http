#pragma once

#include "App.h"
#include "host_bridge.hpp"

#include <memory>
#include <string>
#include <vector>

struct us_listen_socket_t;

namespace mog::http {

struct WebSocketRouteState;
struct WebSocketState;
struct RequestState;
struct ResponseState;

enum class RouteMethod { Get, Head, Post, Put, Patch, Delete, Options, Any };

struct RouteState {
  RouteMethod method = RouteMethod::Get;
  std::string path;
  std::vector<std::string> parameterNames;
  std::unique_ptr<PersistentRoot> callback;
};

struct ServerState : std::enable_shared_from_this<ServerState> {
  explicit ServerState(HostBridge hostBridge);
  ~ServerState();

  bool addRoute(RouteMethod method, std::string path,
                const ExprPackageValue &callback, std::string &error);
  bool setMaxBodySize(int64_t bytes, std::string &error);
  bool listen(std::string host, int port, bool &bound, std::string &error);
  bool run(std::string &error);
  void stop();
  bool isListening() const { return listening; }
  void trackResponse(const std::shared_ptr<ResponseState> &response);
  void trackSocket(const std::shared_ptr<WebSocketState> &socket);

  HostBridge host;
  std::unique_ptr<uWS::App> app;
  us_listen_socket_t *listenSocket = nullptr;
  std::vector<RouteState> routes;
  std::vector<std::shared_ptr<WebSocketRouteState>> webSocketRoutes;
  std::vector<std::weak_ptr<ResponseState>> activeResponses;
  std::vector<std::weak_ptr<WebSocketState>> activeSockets;
  size_t maxBodySize = 1024 * 1024;
  bool listening = false;
  bool running = false;
  bool ran = false;
  bool stopping = false;
  bool stopped = false;
  size_t callbackDepth = 0;

private:
  void dispatch(size_t routeIndex, uWS::HttpResponse<false> *response,
                uWS::HttpRequest *request) noexcept;
  void invokeHttp(size_t routeIndex,
                  const std::shared_ptr<RequestState> &request,
                  const std::shared_ptr<ResponseState> &response) noexcept;
  void closeNow();
  void invalidateActiveStates();
  void releaseCallbacks();
};

} // namespace mog::http
