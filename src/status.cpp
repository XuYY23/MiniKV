#include "minikv/status.h"

namespace minikv {

Status::Status(Code code, std::string message)
    : code_(code), message_(std::move(message)) {}

Status Status::NotFound(const std::string& msg) {
  return Status(Code::kNotFound, msg);
}

Status Status::Corruption(const std::string& msg) {
  return Status(Code::kCorruption, msg);
}

Status Status::IOError(const std::string& msg) {
  return Status(Code::kIOError, msg);
}

Status Status::InvalidArgument(const std::string& msg) {
  return Status(Code::kInvalidArgument, msg);
}

Status Status::NotSupported(const std::string& msg) {
  return Status(Code::kNotSupported, msg);
}

std::string Status::ToString() const {
  switch (code_) {
    case Code::kOk:
      return "OK";
    case Code::kNotFound:
      return "NotFound: " + message_;
    case Code::kCorruption:
      return "Corruption: " + message_;
    case Code::kIOError:
      return "IO error: " + message_;
    case Code::kInvalidArgument:
      return "Invalid argument: " + message_;
    case Code::kNotSupported:
      return "Not supported: " + message_;
  }
  return "Unknown status";
}

}  // namespace minikv
