#pragma once
namespace octopus::core {
class CancellationToken {
 public:
  virtual ~CancellationToken() = default;
  virtual bool isCancellationRequested() const noexcept = 0;
};
}  // namespace octopus::core
