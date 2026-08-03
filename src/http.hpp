#pragma once

#include <cstdint>
#include <memory>
#include <string>
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
void completeAutomaticNoContent(const std::shared_ptr<ResponseState> &response);
void completeInternalError(const std::shared_ptr<ResponseState> &response);
void invalidateResponse(const std::shared_ptr<ResponseState> &response);

} // namespace mog::http
