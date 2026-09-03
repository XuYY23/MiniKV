#pragma once

#include "raft/transport.h"

#include <atomic>
#include <map>
#include <string>

namespace minikv {
namespace raft {

struct Endpoint {
  std::string host;
  uint16_t port = 0;
};

// Synchronous TCP transport. The endpoint map contains every node that may
// participate in the initial or a later joint-consensus configuration.
class TcpRaftTransport : public RaftTransport {
 public:
  explicit TcpRaftTransport(std::map<NodeId, Endpoint> endpoints);

  Status SendRequestVote(NodeId to, const rpc::RaftRequestVoteReq& req,
                         rpc::RaftRequestVoteResp* resp) override;
  Status SendAppendEntries(NodeId to, const rpc::RaftAppendEntriesReq& req,
                           rpc::RaftAppendEntriesResp* resp) override;
  Status SendInstallSnapshot(NodeId to,
                             const rpc::RaftInstallSnapshotReq& req,
                             rpc::RaftInstallSnapshotResp* resp) override;

 private:
  Status Call(NodeId to, rpc::MsgType request_type,
              rpc::MsgType response_type, const std::string& body,
              std::string* response_body);

  std::map<NodeId, Endpoint> endpoints_;
  std::atomic<uint64_t> next_request_id_{1};
};

}  // namespace raft
}  // namespace minikv
