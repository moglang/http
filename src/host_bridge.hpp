#pragma once

#include "NativePackageAPI.hpp"

#include <memory>
#include <string>

namespace mog::http {

class HostBridge {
public:
  HostBridge() = default;
  explicit HostBridge(const ExprHostApi &api) : api_(api) {}

  static bool copyFrom(const ExprHostApi *api, HostBridge &out,
                       std::string &error);

  const ExprHostApi &api() const { return api_; }

private:
  ExprHostApi api_{};
};

class PersistentRoot {
public:
  PersistentRoot(const ExprHostApi &api, ExprPersistentValue *value)
      : api_(api), value_(value) {}
  ~PersistentRoot();

  PersistentRoot(const PersistentRoot &) = delete;
  PersistentRoot &operator=(const PersistentRoot &) = delete;

  bool invoke(const ExprPackageValue *args, size_t argc,
              ExprPackageValue &result, std::string &error) const;
  bool get(ExprPackageValue &result, std::string &error) const;

private:
  ExprHostApi api_{};
  ExprPersistentValue *value_ = nullptr;
};

bool retainRoot(const HostBridge &host, const ExprPackageValue &borrowed,
                std::unique_ptr<PersistentRoot> &out, std::string &error);

} // namespace mog::http
