#include "server.hpp"

#include "handles.hpp"
#include "http.hpp"
#include "validation.hpp"
#include "vendor/uSockets/src/libusockets.h"
#include "websocket.hpp"

#include <algorithm>
#include <charconv>
#include <iostream>
#include <limits>
#include <unordered_map>

namespace mog::http {
namespace {

std::unordered_map<void *, std::weak_ptr<ServerState>> activeServers;

struct CallbackDepthGuard {
  explicit CallbackDepthGuard(size_t &value) : depth(value) { ++depth; }
  ~CallbackDepthGuard() { --depth; }
  size_t &depth;
};

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
    invalidateActiveStates();
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
  if (!routeParameterNames(route.path, route.parameterNames, error))
    return false;
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
  } else if (method == RouteMethod::Head) {
    app->head(routes.back().path, std::move(handler));
  } else if (method == RouteMethod::Post) {
    app->post(routes.back().path, std::move(handler));
  } else if (method == RouteMethod::Put) {
    app->put(routes.back().path, std::move(handler));
  } else if (method == RouteMethod::Patch) {
    app->patch(routes.back().path, std::move(handler));
  } else if (method == RouteMethod::Delete) {
    app->del(routes.back().path, std::move(handler));
  } else if (method == RouteMethod::Options) {
    app->options(routes.back().path, std::move(handler));
  } else {
    app->any(routes.back().path, std::move(handler));
  }
  return true;
}

bool ServerState::setMaxBodySize(int64_t bytes, std::string &error) {
  if (listening || running || ran || stopping || stopped) {
    error = "server body limit must be configured before listen";
    return false;
  }
  if (bytes <= 0 ||
      static_cast<uint64_t>(bytes) >
          static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    error = "maximum body size must be positive and fit usize";
    return false;
  }
  maxBodySize = static_cast<size_t>(bytes);
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
  invalidateActiveStates();
  releaseCallbacks();
  return true;
}

void ServerState::stop() {
  if (stopped || stopping) {
    return;
  }
  stopping = true;
  if (callbackDepth != 0 && running) {
    std::weak_ptr<ServerState> weak = weak_from_this();
    uWS::Loop::get()->defer([weak]() {
      if (auto server = weak.lock())
        server->closeNow();
    });
  } else {
    closeNow();
  }
}

void ServerState::closeNow() {
  if (stopped)
    return;
  listening = false;
  if (app)
    app->close();
  invalidateActiveStates();
  listenSocket = nullptr;
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
    for (const std::string &name : routes[routeIndex].parameterNames) {
      const std::string_view value = nativeRequest->getParameter(name);
      if (value.data() != nullptr)
        request->parameters.emplace(name, std::string(value));
    }
    response = std::make_shared<ResponseState>();
    trackResponse(response);
    response->native = nativeResponse;
    response->headRequest = asciiCaseEqual(request->method, "HEAD");
    nativeResponse->onAborted([response]() {
      response->aborted = true;
      response->native = nullptr;
    });

    const auto contentLength = requestHeader(*request, "content-length");
    const bool chunked =
        requestHeader(*request, "transfer-encoding").has_value();
    size_t declaredLength = 0;
    if (contentLength) {
      const char *begin = contentLength->data();
      const char *end = begin + contentLength->size();
      const auto parsed = std::from_chars(begin, end, declaredLength);
      if (parsed.ec != std::errc() || parsed.ptr != end)
        declaredLength = 0;
    }
    const bool declaredTooLarge = declaredLength > maxBodySize;
    if (chunked || declaredLength != 0) {
      const std::shared_ptr<ServerState> self = shared_from_this();
      nativeResponse->onData([self, routeIndex, request, response,
                              bodyTooLarge = declaredTooLarge](
                                 std::string_view chunk, bool last) mutable {
        if (response->aborted || response->completed)
          return;

        if (!bodyTooLarge &&
            chunk.size() > self->maxBodySize - request->body.size()) {
          request->body.clear();
          bodyTooLarge = true;
        } else if (!bodyTooLarge) {
          request->body.insert(request->body.end(), chunk.begin(), chunk.end());
        }

        if (!last)
          return;
        if (bodyTooLarge) {
          response->completed = true;
          response->native->writeStatus("413 Payload Too Large");
          response->native->writeHeader("Connection", "close");
          response->native->end("Payload Too Large", true);
          invalidateResponse(response);
          return;
        }
        self->invokeHttp(routeIndex, request, response);
      });
      return;
    }
    invokeHttp(routeIndex, request, response);
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

void ServerState::invokeHttp(
    size_t routeIndex, const std::shared_ptr<RequestState> &request,
    const std::shared_ptr<ResponseState> &response) noexcept {
  try {
    ExprPackageValue arguments[] = {
        makeHandleValue(request, "RequestHandle"),
        makeHandleValue(response, "ResponseHandle"),
    };
    ExprPackageValue callbackResult{};
    std::string callbackError;
    CallbackDepthGuard callbackGuard(callbackDepth);
    const bool callbackOk = routes[routeIndex].callback->invoke(
        arguments, 2, callbackResult, callbackError);

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
    }
  } catch (...) {
    std::cerr << "[error][http] unknown native handler exception" << std::endl;
    if (response) {
      completeInternalError(response);
      invalidateResponse(response);
    }
  }
}

void ServerState::trackResponse(
    const std::shared_ptr<ResponseState> &response) {
  activeResponses.erase(
      std::remove_if(activeResponses.begin(), activeResponses.end(),
                     [](const auto &weak) { return weak.expired(); }),
      activeResponses.end());
  activeResponses.emplace_back(response);
}

void ServerState::trackSocket(const std::shared_ptr<WebSocketState> &socket) {
  activeSockets.erase(
      std::remove_if(activeSockets.begin(), activeSockets.end(),
                     [](const auto &weak) { return weak.expired(); }),
      activeSockets.end());
  activeSockets.emplace_back(socket);
}

void ServerState::invalidateActiveStates() {
  for (auto &weak : activeResponses) {
    if (auto response = weak.lock()) {
      if (!response->completed)
        response->aborted = true;
      response->native = nullptr;
    }
  }
  activeResponses.clear();

  for (auto &weak : activeSockets) {
    if (auto socket = weak.lock()) {
      socket->socketData.reset();
      socket->native = nullptr;
      socket->open = false;
      socket->closing = false;
    }
  }
  activeSockets.clear();
}

void ServerState::releaseCallbacks() {
  for (RouteState &route : routes) {
    route.callback.reset();
  }
  for (auto &route : webSocketRoutes) {
    if (route)
      route->releaseCallbacks();
  }
}

} // namespace mog::http
