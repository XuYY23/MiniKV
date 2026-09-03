#pragma once

#include "minikv/status.h"
#include "raft/node.h"
#include "rpc/codec.h"
#include "rpc/message.h"

#include <string>

namespace minikv {
namespace raft {

// 把 RPC payload 交给 RaftNode，生成响应 payload（供 RpcServer handler 使用）。
inline Status HandleRaftRpcPayload(RaftNode* node, const std::string& req_payload,
                                   std::string* resp_payload) {
  if (node == nullptr || resp_payload == nullptr) {
    return Status::InvalidArgument("null");
  }
  rpc::RpcHeader header;
  std::string body;
  Status s = rpc::DecodePayload(req_payload, &header, &body);
  if (!s.ok()) {
    return s;
  }

  if (header.type == rpc::MsgType::kRaftRequestVoteReq) {
    rpc::RaftRequestVoteReq req;
    s = rpc::DecodeRaftRequestVoteReq(body, &req);
    if (!s.ok()) {
      return s;
    }
    rpc::RaftRequestVoteResp resp = node->OnRequestVote(req);
    std::string out_body;
    s = rpc::EncodeRaftRequestVoteResp(resp, &out_body);
    if (!s.ok()) {
      return s;
    }
    rpc::RpcHeader rh = header;
    rh.type = rpc::MsgType::kRaftRequestVoteResp;
    return rpc::EncodePayload(rh, out_body, resp_payload);
  }

  if (header.type == rpc::MsgType::kRaftAppendEntriesReq) {
    rpc::RaftAppendEntriesReq req;
    s = rpc::DecodeRaftAppendEntriesReq(body, &req);
    if (!s.ok()) {
      return s;
    }
    rpc::RaftAppendEntriesResp resp = node->OnAppendEntries(req);
    std::string out_body;
    s = rpc::EncodeRaftAppendEntriesResp(resp, &out_body);
    if (!s.ok()) {
      return s;
    }
    rpc::RpcHeader rh = header;
    rh.type = rpc::MsgType::kRaftAppendEntriesResp;
    return rpc::EncodePayload(rh, out_body, resp_payload);
  }

  if (header.type == rpc::MsgType::kRaftInstallSnapshotReq) {
    rpc::RaftInstallSnapshotReq req;
    s = rpc::DecodeRaftInstallSnapshotReq(body, &req);
    if (!s.ok()) return s;
    rpc::RaftInstallSnapshotResp resp = node->OnInstallSnapshot(req);
    std::string out_body;
    s = rpc::EncodeRaftInstallSnapshotResp(resp, &out_body);
    if (!s.ok()) return s;
    rpc::RpcHeader rh = header;
    rh.type = rpc::MsgType::kRaftInstallSnapshotResp;
    return rpc::EncodePayload(rh, out_body, resp_payload);
  }

  return Status::InvalidArgument("not a raft rpc");
}

}  // namespace raft
}  // namespace minikv
