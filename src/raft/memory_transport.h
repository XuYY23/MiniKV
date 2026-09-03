#pragma once

#include "minikv/status.h"
#include "raft/node.h"
#include "raft/transport.h"

#include <map>
#include <mutex>

namespace minikv {
namespace raft {

// 进程内直调：把 RequestVote / AppendEntries 转到目标 RaftNode。
// 单测与本机复现优先使用；TCP 编解码与 handler 另有用例覆盖。
class MemoryTransport : public RaftTransport {
 public:
  void Register(RaftNode* node) {
    std::lock_guard<std::mutex> lock(mu_);
    nodes_[node->id()] = node;
  }

  void Unregister(NodeId id) {
    std::lock_guard<std::mutex> lock(mu_);
    nodes_.erase(id);
  }

  Status SendRequestVote(NodeId to, const rpc::RaftRequestVoteReq& req,
                         rpc::RaftRequestVoteResp* resp) override {
    RaftNode* node = Find(to);
    if (node == nullptr) {
      return Status::NotFound("peer missing");
    }
    *resp = node->OnRequestVote(req);
    return Status::OK();
  }

  Status SendAppendEntries(NodeId to, const rpc::RaftAppendEntriesReq& req,
                           rpc::RaftAppendEntriesResp* resp) override {
    RaftNode* node = Find(to);
    if (node == nullptr) {
      return Status::NotFound("peer missing");
    }
    *resp = node->OnAppendEntries(req);
    return Status::OK();
  }

  Status SendInstallSnapshot(NodeId to,
                             const rpc::RaftInstallSnapshotReq& req,
                             rpc::RaftInstallSnapshotResp* resp) override {
    RaftNode* node = Find(to);
    if (node == nullptr) return Status::NotFound("peer missing");
    *resp = node->OnInstallSnapshot(req);
    return Status::OK();
  }

 private:
  RaftNode* Find(NodeId id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = nodes_.find(id);
    if (it == nodes_.end()) {
      return nullptr;
    }
    return it->second;
  }

  std::mutex mu_;
  std::map<NodeId, RaftNode*> nodes_;
};

}  // namespace raft
}  // namespace minikv
