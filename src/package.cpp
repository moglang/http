#include "NativePackageAPI.hpp"

#include "handles.hpp"
#include "http.hpp"
#include "server.hpp"
#include "validation.hpp"
#include "websocket.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace mog::http {
namespace {

bool expectCount(const ExprPackageValue *args, size_t argc, size_t expected,
                 ExprPackageStringView *error, const char *functionName) {
  if (argc == expected && (expected == 0 || args != nullptr)) {
    return true;
  }
  return fail(error, std::string(functionName) + " expects " +
                         std::to_string(expected) + " arguments");
}

bool readServer(const ExprPackageValue &value,
                std::shared_ptr<ServerState> &server,
                ExprPackageStringView *error) {
  if (!readHandleBox(value, "ServerHandle", server)) {
    return fail(error, "expected an HTTP Server handle");
  }
  return true;
}

bool readRequest(const ExprPackageValue &value,
                 std::shared_ptr<RequestState> &request,
                 ExprPackageStringView *error) {
  if (!readHandleBox(value, "RequestHandle", request)) {
    return fail(error, "expected an HTTP Request handle");
  }
  return true;
}

bool readResponse(const ExprPackageValue &value,
                  std::shared_ptr<ResponseState> &response,
                  ExprPackageStringView *error) {
  if (!readHandleBox(value, "ResponseHandle", response)) {
    return fail(error, "expected an HTTP Response handle");
  }
  return true;
}

bool readWebSocketRoute(const ExprPackageValue &value,
                        std::shared_ptr<WebSocketRouteState> &route,
                        ExprPackageStringView *error) {
  if (!readHandleBox(value, "WebSocketRouteHandle", route)) {
    return fail(error, "expected a WebSocketRoute handle");
  }
  return true;
}

bool readWebSocket(const ExprPackageValue &value,
                   std::shared_ptr<WebSocketState> &socket,
                   ExprPackageStringView *error) {
  if (!readHandleBox(value, "WebSocketHandle", socket)) {
    return fail(error, "expected a WebSocket handle");
  }
  return true;
}

void returnNull(ExprPackageValue *result) {
  result->kind = EXPR_PACKAGE_VALUE_NULL;
}

void returnString(ExprPackageValue *result, const std::string &value) {
  result->kind = EXPR_PACKAGE_VALUE_STR;
  result->as.string_value = {value.data(), value.size()};
}

void returnOptionalString(ExprPackageValue *result,
                          const std::optional<std::string> &value) {
  static thread_local std::string storage;
  if (!value) {
    returnNull(result);
    return;
  }
  storage = *value;
  returnString(result, storage);
}

bool readBytes(const ExprPackageValue &value, std::string_view &out,
               ExprPackageStringView *error, std::string_view label) {
  if (value.kind != EXPR_PACKAGE_VALUE_BYTES ||
      (value.as.bytes_value.data == nullptr &&
       value.as.bytes_value.length != 0)) {
    return fail(error, std::string(label) + " must be an Array<u8>");
  }
  out = {reinterpret_cast<const char *>(value.as.bytes_value.data),
         value.as.bytes_value.length};
  return true;
}

bool createServer(const ExprHostApi *hostApi, const ExprPackageValue *,
                  size_t argc, ExprPackageValue *result,
                  ExprPackageStringView *error) {
  if (!expectCount(nullptr, argc, 0, error, "createServer") ||
      result == nullptr) {
    return false;
  }
  HostBridge host;
  std::string hostError;
  if (!HostBridge::copyFrom(hostApi, host, hostError)) {
    return fail(error, std::move(hostError));
  }
  *result = makeHandleValue(std::make_shared<ServerState>(std::move(host)),
                            "ServerHandle");
  return true;
}

bool listen(const ExprHostApi *, const ExprPackageValue *args, size_t argc,
            ExprPackageValue *result, ExprPackageStringView *error) {
  if (!expectCount(args, argc, 3, error, "listen") || result == nullptr) {
    return false;
  }
  std::shared_ptr<ServerState> server;
  std::string host;
  int64_t port = 0;
  if (!readServer(args[0], server, error) ||
      !readString(args[1], host, error, "host") ||
      !readI64(args[2], port, error, "port")) {
    return false;
  }
  if (port < 1 || port > 65535) {
    return fail(error, "port must be in 1..65535");
  }
  bool bound = false;
  std::string serverError;
  if (!server->listen(std::move(host), static_cast<int>(port), bound,
                      serverError)) {
    return fail(error, std::move(serverError));
  }
  result->kind = EXPR_PACKAGE_VALUE_BOOL;
  result->as.boolean_value = bound;
  return true;
}

bool setMaxBodySize(const ExprHostApi *, const ExprPackageValue *args,
                    size_t argc, ExprPackageValue *result,
                    ExprPackageStringView *error) {
  if (!expectCount(args, argc, 2, error, "setMaxBodySize") || !result)
    return false;
  std::shared_ptr<ServerState> server;
  int64_t bytes = 0;
  if (!readServer(args[0], server, error) ||
      !readI64(args[1], bytes, error, "maximum body size"))
    return false;
  std::string stateError;
  if (!server->setMaxBodySize(bytes, stateError))
    return fail(error, std::move(stateError));
  returnNull(result);
  return true;
}

bool run(const ExprHostApi *, const ExprPackageValue *args, size_t argc,
         ExprPackageValue *result, ExprPackageStringView *error) {
  if (!expectCount(args, argc, 1, error, "run") || result == nullptr) {
    return false;
  }
  std::shared_ptr<ServerState> server;
  if (!readServer(args[0], server, error)) {
    return false;
  }
  std::string serverError;
  if (!server->run(serverError)) {
    return fail(error, std::move(serverError));
  }
  returnNull(result);
  return true;
}

bool stop(const ExprHostApi *, const ExprPackageValue *args, size_t argc,
          ExprPackageValue *result, ExprPackageStringView *error) {
  if (!expectCount(args, argc, 1, error, "stop") || result == nullptr) {
    return false;
  }
  std::shared_ptr<ServerState> server;
  if (!readServer(args[0], server, error)) {
    return false;
  }
  server->stop();
  returnNull(result);
  return true;
}

bool isListening(const ExprHostApi *, const ExprPackageValue *args, size_t argc,
                 ExprPackageValue *result, ExprPackageStringView *error) {
  if (!expectCount(args, argc, 1, error, "isListening") || result == nullptr) {
    return false;
  }
  std::shared_ptr<ServerState> server;
  if (!readServer(args[0], server, error)) {
    return false;
  }
  result->kind = EXPR_PACKAGE_VALUE_BOOL;
  result->as.boolean_value = server->isListening();
  return true;
}

bool addRoute(RouteMethod method, const char *functionName,
              const ExprPackageValue *args, size_t argc,
              ExprPackageValue *result, ExprPackageStringView *error) {
  if (!expectCount(args, argc, 3, error, functionName) || result == nullptr) {
    return false;
  }
  std::shared_ptr<ServerState> server;
  std::string path;
  if (!readServer(args[0], server, error) ||
      !readString(args[1], path, error, "route path")) {
    return false;
  }
  std::string routeError;
  if (!server->addRoute(method, std::move(path), args[2], routeError)) {
    return fail(error, std::move(routeError));
  }
  returnNull(result);
  return true;
}

bool get(const ExprHostApi *, const ExprPackageValue *args, size_t argc,
         ExprPackageValue *result, ExprPackageStringView *error) {
  return addRoute(RouteMethod::Get, "get", args, argc, result, error);
}

bool head(const ExprHostApi *, const ExprPackageValue *args, size_t argc,
          ExprPackageValue *result, ExprPackageStringView *error) {
  return addRoute(RouteMethod::Head, "head", args, argc, result, error);
}

#define HTTP_ROUTE_FUNCTION(name, method)                                      \
  bool name(const ExprHostApi *, const ExprPackageValue *args, size_t argc,    \
            ExprPackageValue *result, ExprPackageStringView *error) {          \
    return addRoute(RouteMethod::method, #name, args, argc, result, error);    \
  }
HTTP_ROUTE_FUNCTION(post, Post)
HTTP_ROUTE_FUNCTION(put, Put)
HTTP_ROUTE_FUNCTION(patch, Patch)
HTTP_ROUTE_FUNCTION(deleteRoute, Delete)
HTTP_ROUTE_FUNCTION(options, Options)
HTTP_ROUTE_FUNCTION(any, Any)
#undef HTTP_ROUTE_FUNCTION

enum class RequestStringField { Method, Url, Path };

bool requestString(RequestStringField field, const char *functionName,
                   const ExprPackageValue *args, size_t argc,
                   ExprPackageValue *result, ExprPackageStringView *error) {
  if (!expectCount(args, argc, 1, error, functionName) || result == nullptr) {
    return false;
  }
  std::shared_ptr<RequestState> request;
  if (!readRequest(args[0], request, error)) {
    return false;
  }
  switch (field) {
  case RequestStringField::Method:
    returnString(result, request->method);
    return true;
  case RequestStringField::Url:
    returnString(result, request->url);
    return true;
  case RequestStringField::Path:
    returnString(result, request->path);
    return true;
  }
  return fail(error, "unknown request field");
}

bool method(const ExprHostApi *, const ExprPackageValue *args, size_t argc,
            ExprPackageValue *result, ExprPackageStringView *error) {
  return requestString(RequestStringField::Method, "method", args, argc, result,
                       error);
}

bool url(const ExprHostApi *, const ExprPackageValue *args, size_t argc,
         ExprPackageValue *result, ExprPackageStringView *error) {
  return requestString(RequestStringField::Url, "url", args, argc, result,
                       error);
}

bool path(const ExprHostApi *, const ExprPackageValue *args, size_t argc,
          ExprPackageValue *result, ExprPackageStringView *error) {
  return requestString(RequestStringField::Path, "path", args, argc, result,
                       error);
}

enum class RequestLookup { Header, Query, Parameter };

bool requestLookup(RequestLookup kind, const char *name,
                   const ExprPackageValue *args, size_t argc,
                   ExprPackageValue *result, ExprPackageStringView *error) {
  if (!expectCount(args, argc, 2, error, name) || !result)
    return false;
  std::shared_ptr<RequestState> request;
  std::string key;
  if (!readRequest(args[0], request, error) ||
      !readString(args[1], key, error, "lookup name"))
    return false;
  if (kind == RequestLookup::Header)
    returnOptionalString(result, requestHeader(*request, key));
  else if (kind == RequestLookup::Query)
    returnOptionalString(result, requestQuery(*request, key));
  else
    returnOptionalString(result, requestParameter(*request, key));
  return true;
}

bool headerValue(const ExprHostApi *, const ExprPackageValue *args, size_t argc,
                 ExprPackageValue *result, ExprPackageStringView *error) {
  return requestLookup(RequestLookup::Header, "headerValue", args, argc, result,
                       error);
}
bool query(const ExprHostApi *, const ExprPackageValue *args, size_t argc,
           ExprPackageValue *result, ExprPackageStringView *error) {
  return requestLookup(RequestLookup::Query, "query", args, argc, result,
                       error);
}
bool param(const ExprHostApi *, const ExprPackageValue *args, size_t argc,
           ExprPackageValue *result, ExprPackageStringView *error) {
  return requestLookup(RequestLookup::Parameter, "param", args, argc, result,
                       error);
}

bool requestRemoteAddress(const ExprHostApi *, const ExprPackageValue *args,
                          size_t argc, ExprPackageValue *result,
                          ExprPackageStringView *error) {
  if (!expectCount(args, argc, 1, error, "remoteAddress") || !result)
    return false;
  std::shared_ptr<RequestState> request;
  if (!readRequest(args[0], request, error))
    return false;
  returnString(result, request->remoteAddress);
  return true;
}

bool bodyText(const ExprHostApi *, const ExprPackageValue *args, size_t argc,
              ExprPackageValue *result, ExprPackageStringView *error) {
  if (!expectCount(args, argc, 1, error, "bodyText") || !result)
    return false;
  std::shared_ptr<RequestState> request;
  if (!readRequest(args[0], request, error))
    return false;
  result->kind = EXPR_PACKAGE_VALUE_STR;
  result->as.string_value = {
      reinterpret_cast<const char *>(request->body.data()),
      request->body.size()};
  return true;
}

bool bodyBytes(const ExprHostApi *, const ExprPackageValue *args, size_t argc,
               ExprPackageValue *result, ExprPackageStringView *error) {
  if (!expectCount(args, argc, 1, error, "bodyBytes") || !result)
    return false;
  std::shared_ptr<RequestState> request;
  if (!readRequest(args[0], request, error))
    return false;
  result->kind = EXPR_PACKAGE_VALUE_BYTES;
  result->as.bytes_value = {request->body.data(), request->body.size()};
  return true;
}

bool status(const ExprHostApi *, const ExprPackageValue *args, size_t argc,
            ExprPackageValue *result, ExprPackageStringView *error) {
  if (!expectCount(args, argc, 2, error, "status") || result == nullptr) {
    return false;
  }
  std::shared_ptr<ResponseState> response;
  int64_t code = 0;
  if (!readResponse(args[0], response, error) ||
      !readI64(args[1], code, error, "status code")) {
    return false;
  }
  std::string responseError;
  if (!stageStatus(response, code, responseError)) {
    return fail(error, std::move(responseError));
  }
  returnNull(result);
  return true;
}

bool header(const ExprHostApi *, const ExprPackageValue *args, size_t argc,
            ExprPackageValue *result, ExprPackageStringView *error) {
  if (!expectCount(args, argc, 3, error, "header") || result == nullptr) {
    return false;
  }
  std::shared_ptr<ResponseState> response;
  std::string name;
  std::string value;
  if (!readResponse(args[0], response, error) ||
      !readString(args[1], name, error, "header name") ||
      !readString(args[2], value, error, "header value")) {
    return false;
  }
  std::string responseError;
  if (!stageHeader(response, std::move(name), std::move(value),
                   responseError)) {
    return fail(error, std::move(responseError));
  }
  returnNull(result);
  return true;
}

bool text(const ExprHostApi *, const ExprPackageValue *args, size_t argc,
          ExprPackageValue *result, ExprPackageStringView *error) {
  if (!expectCount(args, argc, 2, error, "text") || result == nullptr) {
    return false;
  }
  std::shared_ptr<ResponseState> response;
  std::string body;
  if (!readResponse(args[0], response, error) ||
      !readString(args[1], body, error, "response body")) {
    return false;
  }
  std::string responseError;
  if (!completeText(response, std::move(body), responseError)) {
    return fail(error, std::move(responseError));
  }
  returnNull(result);
  return true;
}

bool completeByteResponse(std::string_view contentType, const char *name,
                          const ExprPackageValue *args, size_t argc,
                          ExprPackageValue *result,
                          ExprPackageStringView *error) {
  if (!expectCount(args, argc, 2, error, name) || !result)
    return false;
  std::shared_ptr<ResponseState> response;
  std::string_view body;
  if (!readResponse(args[0], response, error) ||
      !readBytes(args[1], body, error, "response body"))
    return false;
  std::string responseError;
  if (!completeBytes(response, body, contentType, responseError))
    return fail(error, std::move(responseError));
  returnNull(result);
  return true;
}

bool bytes(const ExprHostApi *, const ExprPackageValue *args, size_t argc,
           ExprPackageValue *result, ExprPackageStringView *error) {
  return completeByteResponse("application/octet-stream", "bytes", args, argc,
                              result, error);
}

bool json(const ExprHostApi *, const ExprPackageValue *args, size_t argc,
          ExprPackageValue *result, ExprPackageStringView *error) {
  if (!expectCount(args, argc, 2, error, "json") || !result)
    return false;
  std::shared_ptr<ResponseState> response;
  std::string body;
  if (!readResponse(args[0], response, error) ||
      !readString(args[1], body, error, "JSON text"))
    return false;
  std::string responseError;
  if (!completeBytes(response, body, "application/json; charset=utf-8",
                     responseError))
    return fail(error, std::move(responseError));
  returnNull(result);
  return true;
}

bool redirect(const ExprHostApi *, const ExprPackageValue *args, size_t argc,
              ExprPackageValue *result, ExprPackageStringView *error) {
  if (!expectCount(args, argc, 3, error, "redirect") || !result)
    return false;
  std::shared_ptr<ResponseState> response;
  std::string location;
  int64_t code = 0;
  if (!readResponse(args[0], response, error) ||
      !readString(args[1], location, error, "redirect location") ||
      !readI64(args[2], code, error, "redirect status"))
    return false;
  std::string responseError;
  if (!completeRedirect(response, std::move(location), code, responseError))
    return fail(error, std::move(responseError));
  returnNull(result);
  return true;
}

bool end(const ExprHostApi *, const ExprPackageValue *args, size_t argc,
         ExprPackageValue *result, ExprPackageStringView *error) {
  if (!expectCount(args, argc, 1, error, "end") || !result)
    return false;
  std::shared_ptr<ResponseState> response;
  if (!readResponse(args[0], response, error))
    return false;
  std::string responseError;
  if (!completeEmpty(response, responseError))
    return fail(error, std::move(responseError));
  returnNull(result);
  return true;
}

bool responseFlag(bool completed, const char *functionName,
                  const ExprPackageValue *args, size_t argc,
                  ExprPackageValue *result, ExprPackageStringView *error) {
  if (!expectCount(args, argc, 1, error, functionName) || result == nullptr) {
    return false;
  }
  std::shared_ptr<ResponseState> response;
  if (!readResponse(args[0], response, error)) {
    return false;
  }
  result->kind = EXPR_PACKAGE_VALUE_BOOL;
  result->as.boolean_value =
      completed ? response->completed : response->aborted;
  return true;
}

bool isCompleted(const ExprHostApi *, const ExprPackageValue *args, size_t argc,
                 ExprPackageValue *result, ExprPackageStringView *error) {
  return responseFlag(true, "isCompleted", args, argc, result, error);
}

bool isAborted(const ExprHostApi *, const ExprPackageValue *args, size_t argc,
               ExprPackageValue *result, ExprPackageStringView *error) {
  return responseFlag(false, "isAborted", args, argc, result, error);
}

bool createWebSocketRoute(const ExprHostApi *hostApi, const ExprPackageValue *,
                          size_t argc, ExprPackageValue *result,
                          ExprPackageStringView *error) {
  if (!expectCount(nullptr, argc, 0, error, "createWebSocketRoute") || !result)
    return false;
  HostBridge host;
  std::string hostError;
  if (!HostBridge::copyFrom(hostApi, host, hostError))
    return fail(error, std::move(hostError));
  *result =
      makeHandleValue(std::make_shared<WebSocketRouteState>(std::move(host)),
                      "WebSocketRouteHandle");
  return true;
}

bool setWebSocketCallback(WebSocketCallback kind, const char *name,
                          const ExprPackageValue *args, size_t argc,
                          ExprPackageValue *result,
                          ExprPackageStringView *error) {
  if (!expectCount(args, argc, 2, error, name) || !result)
    return false;
  std::shared_ptr<WebSocketRouteState> route;
  if (!readWebSocketRoute(args[0], route, error))
    return false;
  std::string routeError;
  if (!route->setCallback(kind, args[1], routeError))
    return fail(error, std::move(routeError));
  returnNull(result);
  return true;
}

#define WS_CALLBACK_FUNCTION(name, kind)                                       \
  bool name(const ExprHostApi *, const ExprPackageValue *args, size_t argc,    \
            ExprPackageValue *result, ExprPackageStringView *error) {          \
    return setWebSocketCallback(WebSocketCallback::kind, #name, args, argc,    \
                                result, error);                                \
  }
WS_CALLBACK_FUNCTION(onOpen, Open)
WS_CALLBACK_FUNCTION(onText, Text)
WS_CALLBACK_FUNCTION(onBinary, Binary)
WS_CALLBACK_FUNCTION(onDrain, Drain)
WS_CALLBACK_FUNCTION(onClose, Close)
#undef WS_CALLBACK_FUNCTION

enum class WebSocketNumberSetting { Payload, Backpressure, IdleTimeout };

bool setWebSocketNumber(WebSocketNumberSetting setting, const char *name,
                        const ExprPackageValue *args, size_t argc,
                        ExprPackageValue *result,
                        ExprPackageStringView *error) {
  if (!expectCount(args, argc, 2, error, name) || !result)
    return false;
  std::shared_ptr<WebSocketRouteState> route;
  int64_t value = 0;
  if (!readWebSocketRoute(args[0], route, error) ||
      !readI64(args[1], value, error, "WebSocket setting"))
    return false;
  std::string routeError;
  bool ok = setting == WebSocketNumberSetting::Payload
                ? route->configurePayload(value, routeError)
            : setting == WebSocketNumberSetting::Backpressure
                ? route->configureBackpressure(value, routeError)
                : route->configureIdleTimeout(value, routeError);
  if (!ok)
    return fail(error, std::move(routeError));
  returnNull(result);
  return true;
}

bool setMaxPayloadLength(const ExprHostApi *, const ExprPackageValue *args,
                         size_t argc, ExprPackageValue *result,
                         ExprPackageStringView *error) {
  return setWebSocketNumber(WebSocketNumberSetting::Payload,
                            "setMaxPayloadLength", args, argc, result, error);
}
bool setMaxBackpressure(const ExprHostApi *, const ExprPackageValue *args,
                        size_t argc, ExprPackageValue *result,
                        ExprPackageStringView *error) {
  return setWebSocketNumber(WebSocketNumberSetting::Backpressure,
                            "setMaxBackpressure", args, argc, result, error);
}
bool setIdleTimeout(const ExprHostApi *, const ExprPackageValue *args,
                    size_t argc, ExprPackageValue *result,
                    ExprPackageStringView *error) {
  return setWebSocketNumber(WebSocketNumberSetting::IdleTimeout,
                            "setIdleTimeout", args, argc, result, error);
}
bool setCloseOnBackpressureLimit(const ExprHostApi *,
                                 const ExprPackageValue *args, size_t argc,
                                 ExprPackageValue *result,
                                 ExprPackageStringView *error) {
  if (!expectCount(args, argc, 2, error, "setCloseOnBackpressureLimit") ||
      !result)
    return false;
  std::shared_ptr<WebSocketRouteState> route;
  if (!readWebSocketRoute(args[0], route, error))
    return false;
  if (args[1].kind != EXPR_PACKAGE_VALUE_BOOL)
    return fail(error, "enabled must be a bool");
  std::string routeError;
  if (!route->configureCloseOnLimit(args[1].as.boolean_value, routeError))
    return fail(error, std::move(routeError));
  returnNull(result);
  return true;
}

bool websocket(const ExprHostApi *, const ExprPackageValue *args, size_t argc,
               ExprPackageValue *result, ExprPackageStringView *error) {
  if (!expectCount(args, argc, 3, error, "websocket") || !result)
    return false;
  std::shared_ptr<ServerState> server;
  std::shared_ptr<WebSocketRouteState> route;
  std::string path;
  if (!readServer(args[0], server, error) ||
      !readString(args[1], path, error, "WebSocket route path") ||
      !readWebSocketRoute(args[2], route, error))
    return false;
  std::string routeError;
  if (!registerWebSocket(server, std::move(path), route, routeError))
    return fail(error, std::move(routeError));
  returnNull(result);
  return true;
}

bool sendSocket(bool textMessage, const char *name,
                const ExprPackageValue *args, size_t argc,
                ExprPackageValue *result, ExprPackageStringView *error) {
  if (!expectCount(args, argc, 2, error, name) || !result)
    return false;
  std::shared_ptr<WebSocketState> socket;
  std::string owned;
  std::string_view message;
  if (!readWebSocket(args[0], socket, error))
    return false;
  if (textMessage) {
    if (!readString(args[1], owned, error, "WebSocket text message"))
      return false;
    if (!validUtf8(owned))
      return fail(error, "WebSocket text message must be valid UTF-8");
    message = owned;
  } else if (!readBytes(args[1], message, error, "WebSocket binary message")) {
    return false;
  }
  std::string socketError;
  const int64_t status = sendWebSocket(
      socket, message, textMessage ? uWS::TEXT : uWS::BINARY, socketError);
  if (status < 0)
    return fail(error, std::move(socketError));
  result->kind = EXPR_PACKAGE_VALUE_I64;
  result->as.i64_value = status;
  return true;
}

bool sendText(const ExprHostApi *, const ExprPackageValue *args, size_t argc,
              ExprPackageValue *result, ExprPackageStringView *error) {
  return sendSocket(true, "sendText", args, argc, result, error);
}
bool sendBinary(const ExprHostApi *, const ExprPackageValue *args, size_t argc,
                ExprPackageValue *result, ExprPackageStringView *error) {
  return sendSocket(false, "sendBinary", args, argc, result, error);
}

bool closeSocket(const ExprHostApi *, const ExprPackageValue *args, size_t argc,
                 ExprPackageValue *result, ExprPackageStringView *error) {
  if (!expectCount(args, argc, 3, error, "close") || !result)
    return false;
  std::shared_ptr<WebSocketState> socket;
  int64_t code = 0;
  std::string reason;
  if (!readWebSocket(args[0], socket, error) ||
      !readI64(args[1], code, error, "WebSocket close code") ||
      !readString(args[2], reason, error, "WebSocket close reason"))
    return false;
  std::string socketError;
  if (!requireOpenSocket(socket, socketError))
    return fail(error, std::move(socketError));
  if (!validWebSocketCloseCode(code))
    return fail(error, "invalid WebSocket close code");
  if (reason.size() > 123)
    return fail(error, "WebSocket close reason must be at most 123 bytes");
  if (!validUtf8(reason))
    return fail(error, "WebSocket close reason must be valid UTF-8");
  socket->closing = true;
  socket->native->end(static_cast<int>(code), reason);
  returnNull(result);
  return true;
}

bool bufferedAmount(const ExprHostApi *, const ExprPackageValue *args,
                    size_t argc, ExprPackageValue *result,
                    ExprPackageStringView *error) {
  if (!expectCount(args, argc, 1, error, "bufferedAmount") || !result)
    return false;
  std::shared_ptr<WebSocketState> socket;
  if (!readWebSocket(args[0], socket, error))
    return false;
  result->kind = EXPR_PACKAGE_VALUE_I64;
  result->as.i64_value =
      socket->native && socket->open
          ? static_cast<int64_t>(socket->native->getBufferedAmount())
          : 0;
  return true;
}

enum class TopicOperation { Subscribe, Unsubscribe, IsSubscribed };
bool topicOperation(TopicOperation operation, const char *name,
                    const ExprPackageValue *args, size_t argc,
                    ExprPackageValue *result, ExprPackageStringView *error) {
  if (!expectCount(args, argc, 2, error, name) || !result)
    return false;
  std::shared_ptr<WebSocketState> socket;
  std::string topic;
  if (!readWebSocket(args[0], socket, error) ||
      !readString(args[1], topic, error, "topic"))
    return false;
  std::string socketError;
  if (!requireOpenSocket(socket, socketError))
    return fail(error, std::move(socketError));
  result->kind = EXPR_PACKAGE_VALUE_BOOL;
  if (operation == TopicOperation::Subscribe)
    result->as.boolean_value = socket->native->subscribe(topic);
  else if (operation == TopicOperation::Unsubscribe)
    result->as.boolean_value = socket->native->unsubscribe(topic);
  else
    result->as.boolean_value = socket->native->isSubscribed(topic);
  return true;
}
bool subscribe(const ExprHostApi *, const ExprPackageValue *args, size_t argc,
               ExprPackageValue *result, ExprPackageStringView *error) {
  return topicOperation(TopicOperation::Subscribe, "subscribe", args, argc,
                        result, error);
}
bool unsubscribe(const ExprHostApi *, const ExprPackageValue *args, size_t argc,
                 ExprPackageValue *result, ExprPackageStringView *error) {
  return topicOperation(TopicOperation::Unsubscribe, "unsubscribe", args, argc,
                        result, error);
}
bool isSubscribed(const ExprHostApi *, const ExprPackageValue *args,
                  size_t argc, ExprPackageValue *result,
                  ExprPackageStringView *error) {
  return topicOperation(TopicOperation::IsSubscribed, "isSubscribed", args,
                        argc, result, error);
}

bool publishSocket(bool textMessage, const char *name,
                   const ExprPackageValue *args, size_t argc,
                   ExprPackageValue *result, ExprPackageStringView *error) {
  if (!expectCount(args, argc, 3, error, name) || !result)
    return false;
  std::shared_ptr<WebSocketState> socket;
  std::string topic, owned;
  std::string_view message;
  if (!readWebSocket(args[0], socket, error) ||
      !readString(args[1], topic, error, "topic"))
    return false;
  if (textMessage) {
    if (!readString(args[2], owned, error, "published text"))
      return false;
    if (!validUtf8(owned))
      return fail(error, "published WebSocket text must be valid UTF-8");
    message = owned;
  } else if (!readBytes(args[2], message, error, "published binary"))
    return false;
  std::string socketError;
  if (!requireOpenSocket(socket, socketError))
    return fail(error, std::move(socketError));
  result->kind = EXPR_PACKAGE_VALUE_BOOL;
  result->as.boolean_value = socket->native->publish(
      topic, message, textMessage ? uWS::TEXT : uWS::BINARY, false);
  return true;
}
bool publishText(const ExprHostApi *, const ExprPackageValue *args, size_t argc,
                 ExprPackageValue *result, ExprPackageStringView *error) {
  return publishSocket(true, "publishText", args, argc, result, error);
}
bool publishBinary(const ExprHostApi *, const ExprPackageValue *args,
                   size_t argc, ExprPackageValue *result,
                   ExprPackageStringView *error) {
  return publishSocket(false, "publishBinary", args, argc, result, error);
}

bool remoteAddressSocket(const ExprHostApi *, const ExprPackageValue *args,
                         size_t argc, ExprPackageValue *result,
                         ExprPackageStringView *error) {
  if (!expectCount(args, argc, 1, error, "remoteAddressSocket") || !result)
    return false;
  std::shared_ptr<WebSocketState> socket;
  if (!readWebSocket(args[0], socket, error))
    return false;
  std::string socketError;
  if (!requireOpenSocket(socket, socketError))
    return fail(error, std::move(socketError));
  returnString(result, socket->remoteAddress);
  return true;
}
bool isOpen(const ExprHostApi *, const ExprPackageValue *args, size_t argc,
            ExprPackageValue *result, ExprPackageStringView *error) {
  if (!expectCount(args, argc, 1, error, "isOpen") || !result)
    return false;
  std::shared_ptr<WebSocketState> socket;
  if (!readWebSocket(args[0], socket, error))
    return false;
  result->kind = EXPR_PACKAGE_VALUE_BOOL;
  result->as.boolean_value = socket->open && !socket->closing && socket->native;
  return true;
}
bool setSocketData(const ExprHostApi *, const ExprPackageValue *args,
                   size_t argc, ExprPackageValue *result,
                   ExprPackageStringView *error) {
  if (!expectCount(args, argc, 2, error, "setSocketData") || !result)
    return false;
  std::shared_ptr<WebSocketState> socket;
  if (!readWebSocket(args[0], socket, error))
    return false;
  std::string socketError;
  if (!requireOpenSocket(socket, socketError))
    return fail(error, std::move(socketError));
  std::unique_ptr<PersistentRoot> replacement;
  if (!retainRoot(socket->route->host, args[1], replacement, socketError))
    return fail(error, std::move(socketError));
  socket->socketData = std::move(replacement);
  returnNull(result);
  return true;
}
bool socketData(const ExprHostApi *, const ExprPackageValue *args, size_t argc,
                ExprPackageValue *result, ExprPackageStringView *error) {
  if (!expectCount(args, argc, 1, error, "socketData") || !result)
    return false;
  std::shared_ptr<WebSocketState> socket;
  if (!readWebSocket(args[0], socket, error))
    return false;
  std::string socketError;
  if (!socket->open && !socket->closing) {
    return fail(error, "WebSocket data is no longer available");
  }
  if (!socket->socketData) {
    returnNull(result);
    return true;
  }
  if (!socket->socketData->get(*result, socketError))
    return fail(error, std::move(socketError));
  return true;
}

constexpr const char *httpRouteSignature =
    "fn(handle<github:http:ServerHandle>, str, "
    "fn(handle<github:http:RequestHandle>, "
    "handle<github:http:ResponseHandle>) -> void) -> void";

constexpr ExprPackageFunctionExport functions[] = {
    {"createServer", "fn() -> handle<github:http:ServerHandle>", 0,
     createServer},
    {"setMaxBodySize", "fn(handle<github:http:ServerHandle>, i64) -> void", 2,
     setMaxBodySize},
    {"listen", "fn(handle<github:http:ServerHandle>, str, i64) -> bool", 3,
     listen},
    {"run", "fn(handle<github:http:ServerHandle>) -> void", 1, run},
    {"stop", "fn(handle<github:http:ServerHandle>) -> void", 1, stop},
    {"isListening", "fn(handle<github:http:ServerHandle>) -> bool", 1,
     isListening},
    {"get", httpRouteSignature, 3, get},
    {"head", httpRouteSignature, 3, head},
    {"post", httpRouteSignature, 3, post},
    {"put", httpRouteSignature, 3, put},
    {"patch", httpRouteSignature, 3, patch},
    {"delete", httpRouteSignature, 3, deleteRoute},
    {"options", httpRouteSignature, 3, options},
    {"any", httpRouteSignature, 3, any},
    {"method", "fn(handle<github:http:RequestHandle>) -> str", 1, method},
    {"url", "fn(handle<github:http:RequestHandle>) -> str", 1, url},
    {"path", "fn(handle<github:http:RequestHandle>) -> str", 1, path},
    {"headerValue", "fn(handle<github:http:RequestHandle>, str) -> str?", 2,
     headerValue},
    {"query", "fn(handle<github:http:RequestHandle>, str) -> str?", 2, query},
    {"param", "fn(handle<github:http:RequestHandle>, str) -> str?", 2, param},
    {"remoteAddress", "fn(handle<github:http:RequestHandle>) -> str", 1,
     requestRemoteAddress},
    {"bodyText", "fn(handle<github:http:RequestHandle>) -> str", 1, bodyText},
    {"bodyBytes", "fn(handle<github:http:RequestHandle>) -> Array<u8>", 1,
     bodyBytes},
    {"status", "fn(handle<github:http:ResponseHandle>, i64) -> void", 2,
     status},
    {"header", "fn(handle<github:http:ResponseHandle>, str, str) -> void", 3,
     header},
    {"text", "fn(handle<github:http:ResponseHandle>, str) -> void", 2, text},
    {"bytes", "fn(handle<github:http:ResponseHandle>, Array<u8>) -> void", 2,
     bytes},
    {"json", "fn(handle<github:http:ResponseHandle>, str) -> void", 2, json},
    {"redirect", "fn(handle<github:http:ResponseHandle>, str, i64) -> void", 3,
     redirect},
    {"end", "fn(handle<github:http:ResponseHandle>) -> void", 1, end},
    {"isCompleted", "fn(handle<github:http:ResponseHandle>) -> bool", 1,
     isCompleted},
    {"isAborted", "fn(handle<github:http:ResponseHandle>) -> bool", 1,
     isAborted},
    {"createWebSocketRoute", "fn() -> handle<github:http:WebSocketRouteHandle>",
     0, createWebSocketRoute},
    {"onOpen",
     "fn(handle<github:http:WebSocketRouteHandle>, "
     "fn(handle<github:http:WebSocketHandle>, "
     "handle<github:http:RequestHandle>) -> void) -> void",
     2, onOpen},
    {"onText",
     "fn(handle<github:http:WebSocketRouteHandle>, "
     "fn(handle<github:http:WebSocketHandle>, str) -> void) -> void",
     2, onText},
    {"onBinary",
     "fn(handle<github:http:WebSocketRouteHandle>, "
     "fn(handle<github:http:WebSocketHandle>, Array<u8>) -> void) -> void",
     2, onBinary},
    {"onDrain",
     "fn(handle<github:http:WebSocketRouteHandle>, "
     "fn(handle<github:http:WebSocketHandle>) -> void) -> void",
     2, onDrain},
    {"onClose",
     "fn(handle<github:http:WebSocketRouteHandle>, "
     "fn(handle<github:http:WebSocketHandle>, i64, str) -> void) -> void",
     2, onClose},
    {"setMaxPayloadLength",
     "fn(handle<github:http:WebSocketRouteHandle>, i64) -> void", 2,
     setMaxPayloadLength},
    {"setMaxBackpressure",
     "fn(handle<github:http:WebSocketRouteHandle>, i64) -> void", 2,
     setMaxBackpressure},
    {"setCloseOnBackpressureLimit",
     "fn(handle<github:http:WebSocketRouteHandle>, bool) -> void", 2,
     setCloseOnBackpressureLimit},
    {"setIdleTimeout",
     "fn(handle<github:http:WebSocketRouteHandle>, i64) -> void", 2,
     setIdleTimeout},
    {"websocket",
     "fn(handle<github:http:ServerHandle>, str, "
     "handle<github:http:WebSocketRouteHandle>) -> void",
     3, websocket},
    {"sendText", "fn(handle<github:http:WebSocketHandle>, str) -> i64", 2,
     sendText},
    {"sendBinary", "fn(handle<github:http:WebSocketHandle>, Array<u8>) -> i64",
     2, sendBinary},
    {"close", "fn(handle<github:http:WebSocketHandle>, i64, str) -> void", 3,
     closeSocket},
    {"bufferedAmount", "fn(handle<github:http:WebSocketHandle>) -> i64", 1,
     bufferedAmount},
    {"subscribe", "fn(handle<github:http:WebSocketHandle>, str) -> bool", 2,
     subscribe},
    {"unsubscribe", "fn(handle<github:http:WebSocketHandle>, str) -> bool", 2,
     unsubscribe},
    {"isSubscribed", "fn(handle<github:http:WebSocketHandle>, str) -> bool", 2,
     isSubscribed},
    {"publishText", "fn(handle<github:http:WebSocketHandle>, str, str) -> bool",
     3, publishText},
    {"publishBinary",
     "fn(handle<github:http:WebSocketHandle>, str, Array<u8>) -> bool", 3,
     publishBinary},
    {"remoteAddressSocket", "fn(handle<github:http:WebSocketHandle>) -> str", 1,
     remoteAddressSocket},
    {"isOpen", "fn(handle<github:http:WebSocketHandle>) -> bool", 1, isOpen},
    {"setSocketData", "fn(handle<github:http:WebSocketHandle>, any) -> void", 2,
     setSocketData},
    {"socketData", "fn(handle<github:http:WebSocketHandle>) -> any", 1,
     socketData},
};

ExprPackageValue i64Constant(int64_t value) {
  ExprPackageValue result{};
  result.kind = EXPR_PACKAGE_VALUE_I64;
  result.as.i64_value = value;
  return result;
}

const ExprPackageConstantExport constants[] = {
    {"SEND_SUCCESS", "i64", i64Constant(0)},
    {"SEND_BACKPRESSURE", "i64", i64Constant(1)},
    {"SEND_DROPPED", "i64", i64Constant(2)},
};

const ExprPackageRegistration registration = {
    EXPR_NATIVE_PACKAGE_ABI_VERSION,
    "github",
    "http",
    functions,
    sizeof(functions) / sizeof(functions[0]),
    constants,
    sizeof(constants) / sizeof(constants[0]),
};

} // namespace
} // namespace mog::http

extern "C" const ExprPackageRegistration *exprRegisterPackage(void) {
  return &mog::http::registration;
}
