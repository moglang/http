#include "http.hpp"

#include "App.h"
#include "validation.hpp"

#include <iostream>
#include <string_view>

namespace mog::http {
namespace {

bool ensureMutable(const std::shared_ptr<ResponseState> &response,
                   std::string &error) {
  if (!response || response->native == nullptr || response->completed ||
      response->aborted) {
    error = "response is no longer active";
    return false;
  }
  return true;
}

std::string statusLine(int code) {
  switch (code) {
  case 100:
    return "100 Continue";
  case 200:
    return "200 OK";
  case 201:
    return "201 Created";
  case 202:
    return "202 Accepted";
  case 204:
    return "204 No Content";
  case 301:
    return "301 Moved Permanently";
  case 302:
    return "302 Found";
  case 304:
    return "304 Not Modified";
  case 400:
    return "400 Bad Request";
  case 404:
    return "404 Not Found";
  case 405:
    return "405 Method Not Allowed";
  case 413:
    return "413 Payload Too Large";
  case 500:
    return "500 Internal Server Error";
  default:
    return std::to_string(code) + " Status";
  }
}

bool hasHeader(const ResponseState &response, std::string_view name) {
  for (const auto &header : response.headers) {
    if (asciiCaseEqual(header.first, name)) {
      return true;
    }
  }
  return false;
}

void writeStagedHead(ResponseState &response, int code) {
  response.native->writeStatus(statusLine(code));
  for (const auto &header : response.headers) {
    response.native->writeHeader(header.first, header.second);
  }
}

} // namespace

std::shared_ptr<RequestState> copyRequest(uWS::HttpRequest *request,
                                          uWS::HttpResponse<false> *response) {
  auto snapshot = std::make_shared<RequestState>();
  const std::string_view method = request->getCaseSensitiveMethod();
  const std::string_view fullUrl = request->getFullUrl();
  const std::string_view path = request->getUrl();
  const std::string_view remote = response->getRemoteAddressAsText();
  snapshot->method.assign(method.data(), method.size());
  snapshot->url.assign(fullUrl.data(), fullUrl.size());
  snapshot->path.assign(path.data(), path.size());
  snapshot->remoteAddress.assign(remote.data(), remote.size());
  for (const auto &header : *request) {
    snapshot->headers.emplace_back(std::string(header.first),
                                   std::string(header.second));
  }
  return snapshot;
}

bool stageStatus(const std::shared_ptr<ResponseState> &response, int64_t code,
                 std::string &error) {
  if (!ensureMutable(response, error)) {
    return false;
  }
  if (code < 100 || code > 599) {
    error = "response status must be in 100..599";
    return false;
  }
  response->statusCode = static_cast<int>(code);
  return true;
}

bool stageHeader(const std::shared_ptr<ResponseState> &response,
                 std::string name, std::string value, std::string &error) {
  if (!ensureMutable(response, error)) {
    return false;
  }
  if (name.empty() || containsCrLf(name) || containsCrLf(value)) {
    error = "response header names must be non-empty and headers cannot "
            "contain CR/LF";
    return false;
  }
  if (asciiCaseEqual(name, "content-length") ||
      asciiCaseEqual(name, "transfer-encoding") ||
      asciiCaseEqual(name, "connection") || asciiCaseEqual(name, "upgrade")) {
    error = "response framing headers are managed by the HTTP package";
    return false;
  }
  for (auto &existing : response->headers) {
    if (asciiCaseEqual(existing.first, name)) {
      existing = {std::move(name), std::move(value)};
      return true;
    }
  }
  response->headers.emplace_back(std::move(name), std::move(value));
  return true;
}

bool completeText(const std::shared_ptr<ResponseState> &response,
                  std::string body, std::string &error) {
  if (!ensureMutable(response, error)) {
    return false;
  }
  if ((response->statusCode >= 100 && response->statusCode < 200) ||
      response->statusCode == 204 || response->statusCode == 304) {
    error = "response status does not permit a body";
    return false;
  }
  if (!hasHeader(*response, "content-type")) {
    response->headers.emplace_back("Content-Type", "text/plain; charset=utf-8");
  }
  response->completed = true;
  writeStagedHead(*response, response->statusCode);
  if (response->headRequest) {
    response->native->endWithoutBody(body.size());
  } else {
    response->native->end(body);
  }
  return true;
}

void completeAutomaticNoContent(
    const std::shared_ptr<ResponseState> &response) {
  if (!response || response->native == nullptr || response->completed ||
      response->aborted) {
    return;
  }
  response->completed = true;
  writeStagedHead(*response, 204);
  response->native->endWithoutBody();
}

void completeInternalError(const std::shared_ptr<ResponseState> &response) {
  if (!response || response->native == nullptr || response->completed ||
      response->aborted) {
    return;
  }
  static constexpr std::string_view body = "Internal Server Error";
  response->headers.clear();
  response->headers.emplace_back("Content-Type", "text/plain; charset=utf-8");
  response->completed = true;
  writeStagedHead(*response, 500);
  if (response->headRequest) {
    response->native->endWithoutBody(body.size());
  } else {
    response->native->end(body);
  }
}

void invalidateResponse(const std::shared_ptr<ResponseState> &response) {
  if (response) {
    response->native = nullptr;
  }
}

} // namespace mog::http
