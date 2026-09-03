#include "rpc/server.h"

#include "rpc/client.h"
#include "rpc/codec.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

namespace minikv {
namespace rpc {

RpcServer::RpcServer() = default;

RpcServer::~RpcServer() { Stop(); }

void RpcServer::SetHandler(RpcHandler handler) { handler_ = std::move(handler); }

Status RpcServer::Start(const std::string& host, uint16_t port) {
  if (running_.load()) {
    return Status::InvalidArgument("server already running");
  }
  if (!handler_) {
    return Status::InvalidArgument("handler not set");
  }

  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return Status::IOError(std::string("socket: ") + std::strerror(errno));
  }

  int yes = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    ::close(fd);
    return Status::InvalidArgument("bad host address");
  }
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    const std::string err = std::strerror(errno);
    ::close(fd);
    return Status::IOError("bind: " + err);
  }
  if (::listen(fd, 64) < 0) {
    const std::string err = std::strerror(errno);
    ::close(fd);
    return Status::IOError("listen: " + err);
  }

  sockaddr_in bound {};
  socklen_t blen = sizeof(bound);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &blen) == 0) {
    bound_port_ = ntohs(bound.sin_port);
  } else {
    bound_port_ = port;
  }

  listen_fd_ = fd;
  running_.store(true);
  accept_thread_ = std::thread(&RpcServer::AcceptLoop, this);
  return Status::OK();
}

void RpcServer::Stop() {
  if (!running_.exchange(false)) {
    if (listen_fd_ >= 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
    }
    return;
  }
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (accept_thread_.joinable()) {
    accept_thread_.join();
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (int fd : client_fds_) ::shutdown(fd, SHUT_RDWR);
  }
  for (auto& worker : client_threads_) {
    if (worker.joinable()) worker.join();
  }
  client_threads_.clear();
}

void RpcServer::AcceptLoop() {
  while (running_.load()) {
    sockaddr_in peer {};
    socklen_t plen = sizeof(peer);
    const int cfd =
        ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &plen);
    if (cfd < 0) {
      if (!running_.load()) {
        break;
      }
      if (errno == EINTR || errno == EAGAIN) {
        continue;
      }
      // listen_fd 被关闭时也会失败，退出循环。
      break;
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (!running_.load()) {
      ::close(cfd);
      break;
    }
    client_fds_.insert(cfd);
    client_threads_.emplace_back(&RpcServer::ServeClient, this, cfd);
  }
}

void RpcServer::ServeClient(int client_fd) {
  while (running_.load()) {
    std::string req;
    Status s = ReadFrame(client_fd, &req);
    if (!s.ok()) {
      break;
    }
    std::string resp;
    if (handler_) {
      s = handler_(req, &resp);
    } else {
      s = Status::IOError("no handler");
    }
    if (!s.ok()) {
      // Protocol errors terminate the current connection.
      break;
    }
    s = WriteFrame(client_fd, resp);
    if (!s.ok()) {
      break;
    }
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    client_fds_.erase(client_fd);
  }
  ::close(client_fd);
}

}  // namespace rpc
}  // namespace minikv
