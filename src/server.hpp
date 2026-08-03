#pragma once

#include "App.h"
#include "host_bridge.hpp"

#include <memory>
#include <string>
#include <vector>

struct us_listen_socket_t;

namespace mog::http {

enum class RouteMethod { Get, Head };

struct RouteState {
  RouteMethod method = RouteMethod::Get;
  std::string path;
  std::unique_ptr<PersistentRoot> callback;
};

struct ServerState : std::enable_shared_from_this<ServerState> {
  explicit ServerState(HostBridge hostBridge);
  ~ServerState();

  bool addRoute(RouteMethod method, std::string path,
                const ExprPackageValue &callback, std::string &error);
  bool listen(std::string host, int port, bool &bound, std::string &error);
  bool run(std::string &error);
  void stop();
  bool isListening() const { return listening; }

  HostBridge host;
  std::unique_ptr<uWS::App> app;
  us_listen_socket_t *listenSocket = nullptr;
  std::vector<RouteState> routes;
  bool listening = false;
  bool running = false;
  bool ran = false;
  bool stopping = false;
  bool stopped = false;
  size_t callbackDepth = 0;

private:
  void dispatch(size_t routeIndex, uWS::HttpResponse<false> *response,
                uWS::HttpRequest *request) noexcept;
  void releaseCallbacks();
};

} // namespace mog::http
