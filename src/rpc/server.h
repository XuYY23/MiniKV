#pragma once

#include "minikv/status.h"
#include "rpc/message.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace minikv {
namespace rpc {

// 请求/响应处理：输入为完整 payload（含 header），输出完整 payload。
using RpcHandler =
    std::function<Status(const std::string& req_payload, std::string* resp_payload)>;

class RpcServer {
 public:
  RpcServer();
  ~RpcServer();

  RpcServer(const RpcServer&) = delete;
  RpcServer& operator=(const RpcServer&) = delete;

  void SetHandler(RpcHandler handler);

  // host 例如 "127.0.0.1"；port=0 表示由内核分配临时端口。
  Status Start(const std::string& host, uint16_t port);
  void Stop();

  uint16_t BoundPort() const { return bound_port_; }
  bool Running() const { return running_.load(); }

 private:
  void AcceptLoop();
  void ServeClient(int client_fd);

  RpcHandler handler_;
  std::atomic<bool> running_{false};
  int listen_fd_ = -1;
  uint16_t bound_port_ = 0;
  std::thread accept_thread_;
  std::mutex mu_;
  std::set<int> client_fds_;
  std::vector<std::thread> client_threads_;
};

}  // namespace rpc
}  // namespace minikv
