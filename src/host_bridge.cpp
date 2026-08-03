#include "host_bridge.hpp"

#include <cstddef>

namespace mog::http {
namespace {

std::string copyError(const ExprPackageStringView &error,
                      const char *fallback) {
  if (error.data != nullptr && error.length != 0) {
    return std::string(error.data, error.length);
  }
  return fallback;
}

} // namespace

bool HostBridge::copyFrom(const ExprHostApi *api, HostBridge &out,
                          std::string &error) {
  if (api == nullptr || api->abi_version < 2 ||
      api->struct_size < sizeof(ExprHostApi) || api->context == nullptr ||
      api->retainValue == nullptr || api->releaseValue == nullptr ||
      api->getValue == nullptr || api->invokeValue == nullptr) {
    error = "Mog Host API v2 is required";
    return false;
  }
  out = HostBridge(*api);
  return true;
}

PersistentRoot::~PersistentRoot() {
  if (value_ != nullptr && api_.releaseValue != nullptr) {
    api_.releaseValue(api_.context, value_);
    value_ = nullptr;
  }
}

bool PersistentRoot::invoke(const ExprPackageValue *args, size_t argc,
                            ExprPackageValue &result,
                            std::string &error) const {
  ExprPackageStringView hostError{};
  if (!api_.invokeValue(api_.context, value_, args, argc, &result,
                        &hostError)) {
    error = copyError(hostError, "Mog callback invocation failed");
    return false;
  }
  return true;
}

bool retainRoot(const HostBridge &host, const ExprPackageValue &borrowed,
                std::unique_ptr<PersistentRoot> &out, std::string &error) {
  ExprPersistentValue *retained = nullptr;
  ExprPackageStringView hostError{};
  const ExprHostApi &api = host.api();
  if (!api.retainValue(api.context, &borrowed, &retained, &hostError)) {
    error = copyError(hostError, "Could not retain Mog callback");
    return false;
  }
  out = std::make_unique<PersistentRoot>(api, retained);
  return true;
}

} // namespace mog::http
