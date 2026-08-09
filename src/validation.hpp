#pragma once

#include "NativePackageAPI.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mog::http {

bool fail(ExprPackageStringView *outError, std::string message);
bool readString(const ExprPackageValue &value, std::string &out,
                ExprPackageStringView *outError, std::string_view label);
bool readI64(const ExprPackageValue &value, int64_t &out,
             ExprPackageStringView *outError, std::string_view label);
bool validRoutePattern(const std::string &pattern, std::string &error);
bool routeParameterNames(const std::string &pattern,
                         std::vector<std::string> &names, std::string &error);
bool containsCrLf(std::string_view text);
bool asciiCaseEqual(std::string_view lhs, std::string_view rhs);
bool validUtf8(std::string_view text);
bool validWebSocketCloseCode(int64_t code);

} // namespace mog::http
