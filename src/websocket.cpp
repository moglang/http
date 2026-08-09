#include "websocket.hpp"

#include "handles.hpp"
#include "http.hpp"
#include "server.hpp"
#include "validation.hpp"

#include <iostream>
#include <limits>
#include <vector>

namespace mog::http {
namespace {

std::unique_ptr<PersistentRoot> &callbackSlot(WebSocketRouteState &route,
                                              WebSocketCallback kind) {
  switch (kind) {
  case WebSocketCallback::Open:
    return route.onOpen;
  case WebSocketCallback::Text:
    return route.onText;
  case WebSocketCallback::Binary:
    return route.onBinary;
  case WebSocketCallback::Drain:
    return route.onDrain;
  case WebSocketCallback::Close:
    return route.onClose;
  }
  return route.onOpen;
}

bool mutableRoute(const WebSocketRouteState &route, std::string &error) {
  if (route.registered) {
    error = "WebSocket route is immutable after registration";
    return false;
  }
  return true;
}

bool invoke(const std::shared_ptr<WebSocketState> &socket,
            const PersistentRoot *callback, const ExprPackageValue *arguments,
            size_t count, bool closeOnError) noexcept {
  if (!callback)
    return true;
  try {
    ExprPackageValue result{};
    std::string error;
    auto server = socket->server.lock();
    struct CallbackDepthGuard {
      explicit CallbackDepthGuard(ServerState *owner) : server(owner) {
        if (server)
          ++server->callbackDepth;
      }
      ~CallbackDepthGuard() {
        if (server)
          --server->callbackDepth;
      }
      ServerState *server;
    } callbackGuard(server.get());
    const bool ok = callback->invoke(arguments, count, result, error);
    if (ok)
      return true;
    std::cerr << "[error][http.websocket] " << error << std::endl;
    if (closeOnError && socket->open && !socket->closing && socket->native) {
      socket->closing = true;
      socket->native->end(1011, "handler error");
    }
  } catch (const std::exception &exception) {
    std::cerr << "[error][http.websocket] native callback exception: "
              << exception.what() << std::endl;
    if (closeOnError && socket->open && !socket->closing && socket->native) {
      socket->closing = true;
      socket->native->end(1011, "handler error");
    }
  } catch (...) {
    std::cerr << "[error][http.websocket] unknown native callback exception"
              << std::endl;
    if (closeOnError && socket->open && !socket->closing && socket->native) {
      socket->closing = true;
      socket->native->end(1011, "handler error");
    }
  }
  return false;
}

ExprPackageValue socketHandle(const std::shared_ptr<WebSocketState> &socket) {
  return makeHandleValue(socket, "WebSocketHandle");
}

} // namespace

bool WebSocketRouteState::setCallback(WebSocketCallback kind,
                                      const ExprPackageValue &callback,
                                      std::string &error) {
  if (!mutableRoute(*this, error))
    return false;
  std::unique_ptr<PersistentRoot> replacement;
  if (!retainRoot(host, callback, replacement, error))
    return false;
  callbackSlot(*this, kind) = std::move(replacement);
  return true;
}

bool WebSocketRouteState::configurePayload(int64_t bytes, std::string &error) {
  if (!mutableRoute(*this, error))
    return false;
  if (bytes <= 0 || bytes > std::numeric_limits<uint32_t>::max()) {
    error = "maximum WebSocket payload must be positive and fit uint32";
    return false;
  }
  maxPayloadLength = static_cast<uint32_t>(bytes);
  return true;
}

bool WebSocketRouteState::configureBackpressure(int64_t bytes,
                                                std::string &error) {
  if (!mutableRoute(*this, error))
    return false;
  if (bytes <= 0 || bytes > std::numeric_limits<uint32_t>::max()) {
    error = "maximum WebSocket backpressure must be positive and fit uint32";
    return false;
  }
  maxBackpressure = static_cast<uint32_t>(bytes);
  return true;
}

bool WebSocketRouteState::configureIdleTimeout(int64_t seconds,
                                               std::string &error) {
  if (!mutableRoute(*this, error))
    return false;
  if (seconds != 0 && (seconds < 8 || seconds > 960)) {
    error = "WebSocket idle timeout must be 0 or in 8..960 seconds";
    return false;
  }
  idleTimeout = static_cast<uint16_t>(seconds);
  return true;
}

bool WebSocketRouteState::configureCloseOnLimit(bool enabled,
                                                std::string &error) {
  if (!mutableRoute(*this, error))
    return false;
  closeOnBackpressureLimit = enabled;
  return true;
}

void WebSocketRouteState::releaseCallbacks() {
  onOpen.reset();
  onText.reset();
  onBinary.reset();
  onDrain.reset();
  onClose.reset();
}

bool registerWebSocket(const std::shared_ptr<ServerState> &server,
                       std::string path,
                       const std::shared_ptr<WebSocketRouteState> &route,
                       std::string &error) {
  if (server->listening || server->running || server->ran || server->stopping ||
      server->stopped) {
    error = "WebSocket routes must be registered before listen";
    return false;
  }
  if (!route || route->registered) {
    error = "WebSocket route may be registered only once";
    return false;
  }
  if (!validRoutePattern(path, error))
    return false;
  std::vector<std::string> parameterNames;
  if (!routeParameterNames(path, parameterNames, error))
    return false;
  route->registered = true;
  server->webSocketRoutes.push_back(route);

  uWS::App::WebSocketBehavior<ConnectionData> behavior;
  behavior.compression = uWS::DISABLED;
  behavior.maxPayloadLength = route->maxPayloadLength;
  behavior.maxBackpressure = route->maxBackpressure;
  behavior.closeOnBackpressureLimit = route->closeOnBackpressureLimit;
  behavior.idleTimeout = route->idleTimeout;
  behavior.sendPingsAutomatically = true;

  const std::weak_ptr<ServerState> weakServer = server;
  behavior.upgrade =
      [weakServer, route, parameterNames = std::move(parameterNames)](
          uWS::HttpResponse<false> *response, uWS::HttpRequest *request,
          us_socket_context_t *context) {
        try {
          auto owner = weakServer.lock();
          if (!owner || owner->stopping || owner->stopped) {
            response->writeStatus("503 Service Unavailable")->end();
            return;
          }
          auto socket = std::make_shared<WebSocketState>();
          owner->trackSocket(socket);
          socket->server = owner;
          socket->route = route;
          const std::string_view remote = response->getRemoteAddressAsText();
          socket->remoteAddress.assign(remote.data(), remote.size());
          auto requestSnapshot = copyRequest(request, response);
          for (const std::string &name : parameterNames) {
            const std::string_view value = request->getParameter(name);
            if (value.data() != nullptr)
              requestSnapshot->parameters.emplace(name, std::string(value));
          }
          socket->pendingRequest = std::move(requestSnapshot);
          ConnectionData data{socket};
          response->upgrade<ConnectionData>(
              std::move(data), request->getHeader("sec-websocket-key"),
              request->getHeader("sec-websocket-protocol"),
              request->getHeader("sec-websocket-extensions"), context);
        } catch (const std::exception &exception) {
          std::cerr << "[error][http.websocket] upgrade exception: "
                    << exception.what() << std::endl;
          response->writeStatus("500 Internal Server Error")->end();
        }
      };
  behavior.open = [weakServer, route](NativeWebSocket *native) {
    auto socket = native->getUserData()->state;
    socket->native = native;
    socket->open = true;
    if (route->onOpen) {
      // The upgrade request is no longer accessible here, so retain its
      // snapshot in state. It is installed by the upgrade callback below
      // through pendingRequest.
      ExprPackageValue args[] = {
          socketHandle(socket),
          makeHandleValue(socket->pendingRequest, "RequestHandle")};
      invoke(socket, route->onOpen.get(), args, 2, true);
    }
    socket->pendingRequest.reset();
  };
  behavior.message = [route](NativeWebSocket *native, std::string_view message,
                             uWS::OpCode opcode) {
    auto socket = native->getUserData()->state;
    if (!socket || !socket->open || socket->closing)
      return;
    if (opcode == uWS::TEXT && route->onText) {
      ExprPackageValue args[] = {socketHandle(socket), {}};
      args[1].kind = EXPR_PACKAGE_VALUE_STR;
      args[1].as.string_value = {message.data(), message.size()};
      invoke(socket, route->onText.get(), args, 2, true);
    } else if (opcode == uWS::BINARY && route->onBinary) {
      ExprPackageValue args[] = {socketHandle(socket), {}};
      args[1].kind = EXPR_PACKAGE_VALUE_BYTES;
      args[1].as.bytes_value = {
          reinterpret_cast<const uint8_t *>(message.data()), message.size()};
      invoke(socket, route->onBinary.get(), args, 2, true);
    }
  };
  behavior.drain = [route](NativeWebSocket *native) {
    auto socket = native->getUserData()->state;
    if (!socket || !socket->open || socket->closing || !route->onDrain)
      return;
    ExprPackageValue args[] = {socketHandle(socket)};
    invoke(socket, route->onDrain.get(), args, 1, true);
  };
  behavior.close = [route](NativeWebSocket *native, int code,
                           std::string_view reason) {
    auto socket = native->getUserData()->state;
    if (!socket)
      return;
    socket->closing = true;
    if (route->onClose) {
      ExprPackageValue args[] = {socketHandle(socket), {}, {}};
      args[1].kind = EXPR_PACKAGE_VALUE_I64;
      args[1].as.i64_value = code;
      args[2].kind = EXPR_PACKAGE_VALUE_STR;
      args[2].as.string_value = {reason.data(), reason.size()};
      invoke(socket, route->onClose.get(), args, 3, false);
    }
    socket->socketData.reset();
    socket->native = nullptr;
    socket->open = false;
    socket->closing = false;
  };

  server->app->ws<ConnectionData>(std::move(path), std::move(behavior));
  return true;
}

bool requireOpenSocket(const std::shared_ptr<WebSocketState> &socket,
                       std::string &error) {
  if (!socket || !socket->open || socket->closing ||
      socket->native == nullptr) {
    error = "WebSocket is no longer open";
    return false;
  }
  return true;
}

int64_t sendWebSocket(const std::shared_ptr<WebSocketState> &socket,
                      std::string_view message, uWS::OpCode opcode,
                      std::string &error) {
  if (!requireOpenSocket(socket, error))
    return -1;
  const auto status = socket->native->send(message, opcode, false);
  if (status == NativeWebSocket::SUCCESS)
    return 0;
  if (status == NativeWebSocket::BACKPRESSURE)
    return 1;
  return 2;
}

} // namespace mog::http
