#pragma once

#include <string>

namespace minikv {

// 轻量状态码：成功或携带可读错误信息。
class Status {
 public:
  enum class Code : int {
    kOk = 0,
    kNotFound = 1,
    kCorruption = 2,
    kIOError = 3,
    kInvalidArgument = 4,
    kNotSupported = 5,
  };

  Status() = default;

  static Status OK() { return Status(); }
  static Status NotFound(const std::string& msg);
  static Status Corruption(const std::string& msg);
  static Status IOError(const std::string& msg);
  static Status InvalidArgument(const std::string& msg);
  static Status NotSupported(const std::string& msg);

  bool ok() const { return code_ == Code::kOk; }
  bool IsNotFound() const { return code_ == Code::kNotFound; }
  bool IsCorruption() const { return code_ == Code::kCorruption; }
  bool IsIOError() const { return code_ == Code::kIOError; }
  bool IsInvalidArgument() const { return code_ == Code::kInvalidArgument; }

  Code code() const { return code_; }
  const std::string& message() const { return message_; }
  std::string ToString() const;

 private:
  Status(Code code, std::string message);

  Code code_ = Code::kOk;
  std::string message_;
};

}  // namespace minikv
