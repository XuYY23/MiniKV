#include "rpc/client.h"

#include "rpc/codec.h"
#include "engine/coding.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

namespace minikv {
namespace rpc {
namespace {

Status IoWriteAll(int fd, const char* data, size_t n) {
  size_t off = 0;
  while (off < n) {
    const ssize_t w = ::send(fd, data + off, n - off, MSG_NOSIGNAL);
    if (w < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Status::IOError(std::string("send: ") + std::strerror(errno));
    }
    if (w == 0) {
      return Status::IOError("send returned 0");
    }
    off += static_cast<size_t>(w);
  }
  return Status::OK();
}

Status IoReadExact(int fd, char* data, size_t n) {
  size_t off = 0;
  while (off < n) {
    const ssize_t r = ::recv(fd, data + off, n - off, 0);
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Status::IOError(std::string("recv: ") + std::strerror(errno));
    }
    if (r == 0) {
      return Status::IOError("peer closed");
    }
    off += static_cast<size_t>(r);
  }
  return Status::OK();
}

}  // namespace

Status WriteFrame(int fd, const std::string& payload) {
  std::string frame;
  Status s = EncodeFrame(payload, &frame);
  if (!s.ok()) {
    return s;
  }
  return IoWriteAll(fd, frame.data(), frame.size());
}

Status ReadFrame(int fd, std::string* payload) {
  char lenbuf[4];
  Status s = IoReadExact(fd, lenbuf, 4);
  if (!s.ok()) {
    return s;
  }
  const uint32_t len = DecodeFixed32(lenbuf);
  if (len > kRpcMaxPayload) {
    return Status::Corruption("frame length too large");
  }
  payload->assign(static_cast<size_t>(len), '\0');
  if (len == 0) {
    return Status::OK();
  }
  return IoReadExact(fd, &(*payload)[0], len);
}

RpcClient::RpcClient() = default;

RpcClient::~RpcClient() { Close(); }

void RpcClient::Close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

Status RpcClient::Connect(const std::string& host, uint16_t port) {
  Close();
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return Status::IOError(std::string("socket: ") + std::strerror(errno));
  }
  timeval timeout{};
  timeout.tv_sec = 3;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    ::close(fd);
    return Status::InvalidArgument("bad host address");
  }
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    const std::string err = std::strerror(errno);
    ::close(fd);
    return Status::IOError("connect: " + err);
  }
  fd_ = fd;
  return Status::OK();
}

Status RpcClient::Call(const std::string& req_payload, std::string* resp_payload) {
  if (fd_ < 0) {
    return Status::IOError("client not connected");
  }
  if (resp_payload == nullptr) {
    return Status::InvalidArgument("null resp");
  }
  Status s = WriteFrame(fd_, req_payload);
  if (!s.ok()) {
    return s;
  }
  return ReadFrame(fd_, resp_payload);
}

Status RpcClient::CallPing(const std::string& nonce, std::string* echoed,
                           uint64_t request_id) {
  Ping req;
  req.nonce = nonce;
  std::string body;
  Status s = EncodePing(req, &body);
  if (!s.ok()) {
    return s;
  }
  RpcHeader header;
  header.type = MsgType::kPing;
  header.request_id = request_id;
  std::string payload;
  s = EncodePayload(header, body, &payload);
  if (!s.ok()) {
    return s;
  }
  std::string resp_payload;
  s = Call(payload, &resp_payload);
  if (!s.ok()) {
    return s;
  }
  RpcHeader rh;
  std::string rbody;
  s = DecodePayload(resp_payload, &rh, &rbody);
  if (!s.ok()) {
    return s;
  }
  if (rh.type != MsgType::kPong) {
    return Status::Corruption("expected Pong");
  }
  if (rh.request_id != request_id) {
    return Status::Corruption("request_id mismatch");
  }
  Pong pong;
  s = DecodePong(rbody, &pong);
  if (!s.ok()) {
    return s;
  }
  if (echoed != nullptr) {
    *echoed = pong.nonce;
  }
  return Status::OK();
}

}  // namespace rpc
}  // namespace minikv
