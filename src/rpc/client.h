#pragma once

#include "minikv/status.h"
#include "rpc/message.h"

#include <cstdint>
#include <string>

namespace minikv {
namespace rpc {

class RpcClient {
 public:
  RpcClient();
  ~RpcClient();

  RpcClient(const RpcClient&) = delete;
  RpcClient& operator=(const RpcClient&) = delete;

  Status Connect(const std::string& host, uint16_t port);
  void Close();

  // 发送一帧请求并读回一帧响应（同步调用）。
  Status Call(const std::string& req_payload, std::string* resp_payload);

  // 便捷：Ping/Pong
  Status CallPing(const std::string& nonce, std::string* echoed,
                  uint64_t request_id = 1);

 private:
  int fd_ = -1;
};

// 从已连接 socket 读写一整帧。
Status WriteFrame(int fd, const std::string& payload);
Status ReadFrame(int fd, std::string* payload);

}  // namespace rpc
}  // namespace minikv
