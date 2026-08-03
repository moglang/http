#pragma once

#include "NativePackageAPI.hpp"

#include <memory>
#include <string_view>

namespace mog::http {

template <typename State> struct HandleBox {
  std::shared_ptr<State> state;
};

template <typename State> void releaseHandleBox(void *data) {
  delete static_cast<HandleBox<State> *>(data);
}

template <typename State>
ExprPackageValue makeHandleValue(std::shared_ptr<State> state,
                                 const char *typeName) {
  auto *box = new HandleBox<State>{std::move(state)};
  ExprPackageValue value{};
  value.kind = EXPR_PACKAGE_VALUE_HANDLE;
  value.as.handle_value = {"github", "http", typeName, box,
                           &releaseHandleBox<State>};
  return value;
}

template <typename State>
bool readHandleBox(const ExprPackageValue &value, const char *expectedType,
                   std::shared_ptr<State> &out) {
  if (value.kind != EXPR_PACKAGE_VALUE_HANDLE) {
    return false;
  }
  const ExprPackageHandleValue &handle = value.as.handle_value;
  if (handle.package_namespace == nullptr || handle.package_name == nullptr ||
      handle.type_name == nullptr || handle.handle_data == nullptr ||
      std::string_view(handle.package_namespace) != "github" ||
      std::string_view(handle.package_name) != "http" ||
      std::string_view(handle.type_name) != expectedType) {
    return false;
  }
  auto *box = static_cast<HandleBox<State> *>(handle.handle_data);
  out = box->state;
  return static_cast<bool>(out);
}

} // namespace mog::http
