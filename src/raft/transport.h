#pragma once

#include "minikv/status.h"
#include "raft/types.h"
#include "rpc/message.h"

namespace minikv {
namespace raft {

// 节点间通信抽象：可内存直调，也可挂 TCP RPC。
class RaftTransport {
 public:
  virtual ~RaftTransport() = default;

  virtual Status SendRequestVote(NodeId to, const rpc::RaftRequestVoteReq& req,
                                 rpc::RaftRequestVoteResp* resp) = 0;

  virtual Status SendAppendEntries(NodeId to,
                                   const rpc::RaftAppendEntriesReq& req,
                                   rpc::RaftAppendEntriesResp* resp) = 0;
  virtual Status SendInstallSnapshot(
      NodeId to, const rpc::RaftInstallSnapshotReq& req,
      rpc::RaftInstallSnapshotResp* resp) = 0;
};

}  // namespace raft
}  // namespace minikv
