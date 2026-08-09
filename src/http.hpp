#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace uWS {
template <bool SSL> struct HttpResponse;
struct HttpRequest;
} // namespace uWS

namespace mog::http {

struct RequestState {
  std::string method;
  std::string url;
  std::string path;
  std::string remoteAddress;
  std::vector<std::pair<std::string, std::string>> headers;
  std::unordered_map<std::string, std::string> parameters;
  std::vector<uint8_t> body;
};

struct ResponseState {
  uWS::HttpResponse<false> *native = nullptr;
  int statusCode = 200;
  std::vector<std::pair<std::string, std::string>> headers;
  bool headRequest = false;
  bool completed = false;
  bool aborted = false;
};

std::shared_ptr<RequestState> copyRequest(uWS::HttpRequest *request,
                                          uWS::HttpResponse<false> *response);

bool stageStatus(const std::shared_ptr<ResponseState> &response, int64_t code,
                 std::string &error);
bool stageHeader(const std::shared_ptr<ResponseState> &response,
                 std::string name, std::string value, std::string &error);
bool completeText(const std::shared_ptr<ResponseState> &response,
                  std::string body, std::string &error);
bool completeBytes(const std::shared_ptr<ResponseState> &response,
                   std::string_view body, std::string_view defaultContentType,
                   std::string &error);
bool completeRedirect(const std::shared_ptr<ResponseState> &response,
                      std::string location, int64_t code, std::string &error);
bool completeEmpty(const std::shared_ptr<ResponseState> &response,
                   std::string &error);
std::optional<std::string> requestHeader(const RequestState &request,
                                         std::string_view name);
std::optional<std::string> requestQuery(const RequestState &request,
                                        std::string_view name);
std::optional<std::string> requestParameter(const RequestState &request,
                                            std::string_view name);
void completeAutomaticNoContent(const std::shared_ptr<ResponseState> &response);
void completeInternalError(const std::shared_ptr<ResponseState> &response);
void invalidateResponse(const std::shared_ptr<ResponseState> &response);

} // namespace mog::http
