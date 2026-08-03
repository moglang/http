#include "server.hpp"

#include "handles.hpp"
#include "http.hpp"
#include "validation.hpp"
#include "vendor/uSockets/src/libusockets.h"

#include <iostream>
#include <unordered_map>

namespace mog::http {
namespace {

std::unordered_map<void *, std::weak_ptr<ServerState>> activeServers;

void clearActiveServer(const ServerState &server) {
  const void *context = server.host.api().context;
  auto found = activeServers.find(const_cast<void *>(context));
  if (found == activeServers.end()) {
    return;
  }
  const std::shared_ptr<ServerState> current = found->second.lock();
  if (!current || current.get() == &server) {
    activeServers.erase(found);
  }
}

bool reserveActiveServer(const std::shared_ptr<ServerState> &server,
                         std::string &error) {
  void *context = server->host.api().context;
  auto found = activeServers.find(context);
  if (found != activeServers.end()) {
    const std::shared_ptr<ServerState> current = found->second.lock();
    if (current && current.get() != server.get() && current->isListening()) {
      error = "only one HTTP server may listen in a VM at a time";
      return false;
    }
    activeServers.erase(found);
  }
  activeServers[context] = server;
  return true;
}

} // namespace

ServerState::ServerState(HostBridge hostBridge)
    : host(std::move(hostBridge)), app(std::make_unique<uWS::App>()) {}

ServerState::~ServerState() {
  stop();
  if (app) {
    app->close();
    app.reset();
  }
  releaseCallbacks();
  clearActiveServer(*this);
}

bool ServerState::addRoute(RouteMethod method, std::string path,
                           const ExprPackageValue &callback,
                           std::string &error) {
  if (listening || running || ran || stopping || stopped) {
    error = "routes must be registered before listen";
    return false;
  }
  if (!validRoutePattern(path, error)) {
    return false;
  }

  RouteState route;
  route.method = method;
  route.path = std::move(path);
  if (!retainRoot(host, callback, route.callback, error)) {
    return false;
  }
  const size_t routeIndex = routes.size();
  routes.push_back(std::move(route));
  const std::weak_ptr<ServerState> weak = weak_from_this();
  auto handler = [weak, routeIndex](uWS::HttpResponse<false> *response,
                                    uWS::HttpRequest *request) {
    if (const std::shared_ptr<ServerState> server = weak.lock()) {
      server->dispatch(routeIndex, response, request);
    } else {
      response->writeStatus("500 Internal Server Error");
      response->end("Internal Server Error", true);
    }
  };
  if (method == RouteMethod::Get) {
    app->get(routes.back().path, std::move(handler));
  } else {
    app->head(routes.back().path, std::move(handler));
  }
  return true;
}

bool ServerState::listen(std::string bindHost, int port, bool &bound,
                         std::string &error) {
  bound = false;
  if (listening || running || ran || stopping || stopped) {
    error = "server is not in a listenable state";
    return false;
  }
  if (port < 1 || port > 65535) {
    error = "port must be in 1..65535";
    return false;
  }
  if (bindHost.find('\0') != std::string::npos) {
    error = "host cannot contain an embedded NUL byte";
    return false;
  }
  if (!reserveActiveServer(shared_from_this(), error)) {
    return false;
  }

  app->listen(std::move(bindHost), port,
              [this](us_listen_socket_t *socket) { listenSocket = socket; });
  if (listenSocket == nullptr) {
    clearActiveServer(*this);
    return true;
  }
  listening = true;
  bound = true;
  return true;
}

bool ServerState::run(std::string &error) {
  if (!listening && !stopped) {
    error = "run requires a successful listen";
    return false;
  }
  if (ran || running) {
    error = "server run may be called only once";
    return false;
  }
  ran = true;
  if (!stopped) {
    running = true;
    app->run();
    running = false;
  }
  listening = false;
  stopped = true;
  listenSocket = nullptr;
  clearActiveServer(*this);
  app->close();
  releaseCallbacks();
  return true;
}

void ServerState::stop() {
  if (stopped || stopping) {
    return;
  }
  stopping = true;
  listening = false;
  if (listenSocket != nullptr) {
    us_listen_socket_close(0, listenSocket);
    listenSocket = nullptr;
  }
  stopped = true;
  stopping = false;
  clearActiveServer(*this);
}

void ServerState::dispatch(size_t routeIndex,
                           uWS::HttpResponse<false> *nativeResponse,
                           uWS::HttpRequest *nativeRequest) noexcept {
  std::shared_ptr<ResponseState> response;
  try {
    if (routeIndex >= routes.size() || !routes[routeIndex].callback) {
      nativeResponse->writeStatus("500 Internal Server Error");
      nativeResponse->end("Internal Server Error", true);
      return;
    }

    const std::shared_ptr<RequestState> request =
        copyRequest(nativeRequest, nativeResponse);
    response = std::make_shared<ResponseState>();
    response->native = nativeResponse;
    response->headRequest = routes[routeIndex].method == RouteMethod::Head;
    nativeResponse->onAborted([response]() {
      response->aborted = true;
      response->native = nullptr;
    });

    ExprPackageValue arguments[] = {
        makeHandleValue(request, "RequestHandle"),
        makeHandleValue(response, "ResponseHandle"),
    };
    ExprPackageValue callbackResult{};
    std::string callbackError;
    ++callbackDepth;
    const bool callbackOk = routes[routeIndex].callback->invoke(
        arguments, 2, callbackResult, callbackError);
    --callbackDepth;

    if (!callbackOk) {
      std::cerr << "[error][http] " << callbackError << std::endl;
      completeInternalError(response);
    } else if (!response->completed && !response->aborted) {
      std::cerr << "[warning][http] handler returned without completing "
                   "its response; sending 204"
                << std::endl;
      completeAutomaticNoContent(response);
    }
    invalidateResponse(response);
  } catch (const std::exception &exception) {
    std::cerr << "[error][http] native handler exception: " << exception.what()
              << std::endl;
    if (response) {
      completeInternalError(response);
      invalidateResponse(response);
    } else {
      nativeResponse->writeStatus("500 Internal Server Error");
      nativeResponse->end("Internal Server Error", true);
    }
  } catch (...) {
    std::cerr << "[error][http] unknown native handler exception" << std::endl;
    if (response) {
      completeInternalError(response);
      invalidateResponse(response);
    } else {
      nativeResponse->writeStatus("500 Internal Server Error");
      nativeResponse->end("Internal Server Error", true);
    }
  }
}

void ServerState::releaseCallbacks() {
  for (RouteState &route : routes) {
    route.callback.reset();
  }
}

} // namespace mog::http
