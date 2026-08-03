#include "validation.hpp"

#include <cctype>
#include <unordered_set>

namespace mog::http {

bool fail(ExprPackageStringView *outError, std::string message) {
  static thread_local std::string storage;
  storage = std::move(message);
  if (outError != nullptr) {
    *outError = {storage.data(), storage.size()};
  }
  return false;
}

bool readString(const ExprPackageValue &value, std::string &out,
                ExprPackageStringView *outError, std::string_view label) {
  if (value.kind != EXPR_PACKAGE_VALUE_STR ||
      (value.as.string_value.data == nullptr &&
       value.as.string_value.length != 0)) {
    return fail(outError, std::string(label) + " must be a str");
  }
  if (value.as.string_value.length == 0) {
    out.clear();
  } else {
    out.assign(value.as.string_value.data, value.as.string_value.length);
  }
  return true;
}

bool readI64(const ExprPackageValue &value, int64_t &out,
             ExprPackageStringView *outError, std::string_view label) {
  if (value.kind != EXPR_PACKAGE_VALUE_I64) {
    return fail(outError, std::string(label) + " must be an i64");
  }
  out = value.as.i64_value;
  return true;
}

bool validRoutePattern(const std::string &pattern, std::string &error) {
  if (pattern.empty() || pattern.front() != '/') {
    error = "route path must be non-empty and start with '/'";
    return false;
  }

  std::unordered_set<std::string> parameters;
  for (size_t index = 0; index < pattern.size();) {
    if (pattern[index] != ':') {
      ++index;
      continue;
    }
    const size_t start = ++index;
    while (index < pattern.size() && pattern[index] != '/') {
      ++index;
    }
    const std::string name = pattern.substr(start, index - start);
    if (name.empty()) {
      error = "route parameter names cannot be empty";
      return false;
    }
    if (!parameters.insert(name).second) {
      error = "route contains duplicate parameter '" + name + "'";
      return false;
    }
  }
  return true;
}

bool containsCrLf(std::string_view text) {
  return text.find('\r') != std::string_view::npos ||
         text.find('\n') != std::string_view::npos;
}

bool asciiCaseEqual(std::string_view lhs, std::string_view rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (size_t index = 0; index < lhs.size(); ++index) {
    const unsigned char left = static_cast<unsigned char>(lhs[index]);
    const unsigned char right = static_cast<unsigned char>(rhs[index]);
    if (std::tolower(left) != std::tolower(right)) {
      return false;
    }
  }
  return true;
}

} // namespace mog::http
