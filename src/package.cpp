#include "NativePackageAPI.hpp"

#include "handles.hpp"
#include "http.hpp"
#include "server.hpp"
#include "validation.hpp"

#include <cstdint>
#include <memory>
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

void returnNull(ExprPackageValue *result) {
  result->kind = EXPR_PACKAGE_VALUE_NULL;
}

void returnString(ExprPackageValue *result, const std::string &value) {
  result->kind = EXPR_PACKAGE_VALUE_STR;
  result->as.string_value = {value.data(), value.size()};
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

constexpr ExprPackageFunctionExport functions[] = {
    {"createServer", "fn() -> handle<github:http:ServerHandle>", 0,
     createServer},
    {"listen", "fn(handle<github:http:ServerHandle>, str, i64) -> bool", 3,
     listen},
    {"run", "fn(handle<github:http:ServerHandle>) -> void", 1, run},
    {"stop", "fn(handle<github:http:ServerHandle>) -> void", 1, stop},
    {"isListening", "fn(handle<github:http:ServerHandle>) -> bool", 1,
     isListening},
    {"get",
     "fn(handle<github:http:ServerHandle>, str, "
     "fn(handle<github:http:RequestHandle>, "
     "handle<github:http:ResponseHandle>) -> void) -> void",
     3, get},
    {"head",
     "fn(handle<github:http:ServerHandle>, str, "
     "fn(handle<github:http:RequestHandle>, "
     "handle<github:http:ResponseHandle>) -> void) -> void",
     3, head},
    {"method", "fn(handle<github:http:RequestHandle>) -> str", 1, method},
    {"url", "fn(handle<github:http:RequestHandle>) -> str", 1, url},
    {"path", "fn(handle<github:http:RequestHandle>) -> str", 1, path},
    {"status", "fn(handle<github:http:ResponseHandle>, i64) -> void", 2,
     status},
    {"header", "fn(handle<github:http:ResponseHandle>, str, str) -> void", 3,
     header},
    {"text", "fn(handle<github:http:ResponseHandle>, str) -> void", 2, text},
    {"isCompleted", "fn(handle<github:http:ResponseHandle>) -> bool", 1,
     isCompleted},
    {"isAborted", "fn(handle<github:http:ResponseHandle>) -> bool", 1,
     isAborted},
};

constexpr ExprPackageRegistration registration = {
    EXPR_NATIVE_PACKAGE_ABI_VERSION,          "github", "http", functions,
    sizeof(functions) / sizeof(functions[0]), nullptr,  0,
};

} // namespace
} // namespace mog::http

extern "C" const ExprPackageRegistration *exprRegisterPackage(void) {
  return &mog::http::registration;
}
