#include "raft/tcp_transport.h"

#include "rpc/client.h"
#include "rpc/codec.h"

namespace minikv {
namespace raft {

TcpRaftTransport::TcpRaftTransport(std::map<NodeId, Endpoint> endpoints)
    : endpoints_(std::move(endpoints)) {}

Status TcpRaftTransport::Call(NodeId to, rpc::MsgType request_type,
                              rpc::MsgType response_type,
                              const std::string& body,
                              std::string* response_body) {
  auto it = endpoints_.find(to);
  if (it == endpoints_.end()) return Status::NotFound("Raft peer endpoint");
  rpc::RpcClient client;
  Status s = client.Connect(it->second.host, it->second.port);
  if (!s.ok()) return s;
  rpc::RpcHeader header;
  header.type = request_type;
  header.request_id = next_request_id_.fetch_add(1);
  std::string request_payload;
  s = rpc::EncodePayload(header, body, &request_payload);
  if (!s.ok()) return s;
  std::string response_payload;
  s = client.Call(request_payload, &response_payload);
  if (!s.ok()) return s;
  rpc::RpcHeader response_header;
  s = rpc::DecodePayload(response_payload, &response_header, response_body);
  if (!s.ok()) return s;
  if (response_header.type != response_type ||
      response_header.request_id != header.request_id) {
    return Status::Corruption("unexpected Raft RPC response");
  }
  return Status::OK();
}

Status TcpRaftTransport::SendRequestVote(
    NodeId to, const rpc::RaftRequestVoteReq& req,
    rpc::RaftRequestVoteResp* resp) {
  std::string body;
  Status s = rpc::EncodeRaftRequestVoteReq(req, &body);
  if (!s.ok()) return s;
  std::string response_body;
  s = Call(to, rpc::MsgType::kRaftRequestVoteReq,
           rpc::MsgType::kRaftRequestVoteResp, body, &response_body);
  if (!s.ok()) return s;
  return rpc::DecodeRaftRequestVoteResp(response_body, resp);
}

Status TcpRaftTransport::SendAppendEntries(
    NodeId to, const rpc::RaftAppendEntriesReq& req,
    rpc::RaftAppendEntriesResp* resp) {
  std::string body;
  Status s = rpc::EncodeRaftAppendEntriesReq(req, &body);
  if (!s.ok()) return s;
  std::string response_body;
  s = Call(to, rpc::MsgType::kRaftAppendEntriesReq,
           rpc::MsgType::kRaftAppendEntriesResp, body, &response_body);
  if (!s.ok()) return s;
  return rpc::DecodeRaftAppendEntriesResp(response_body, resp);
}

Status TcpRaftTransport::SendInstallSnapshot(
    NodeId to, const rpc::RaftInstallSnapshotReq& req,
    rpc::RaftInstallSnapshotResp* resp) {
  std::string body;
  Status s = rpc::EncodeRaftInstallSnapshotReq(req, &body);
  if (!s.ok()) return s;
  std::string response_body;
  s = Call(to, rpc::MsgType::kRaftInstallSnapshotReq,
           rpc::MsgType::kRaftInstallSnapshotResp, body, &response_body);
  if (!s.ok()) return s;
  return rpc::DecodeRaftInstallSnapshotResp(response_body, resp);
}

}  // namespace raft
}  // namespace minikv
