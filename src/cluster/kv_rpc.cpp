#include "cluster/kv_rpc.h"

#include "rpc/client.h"
#include "rpc/codec.h"
#include "raft/rpc_bridge.h"

namespace minikv {
namespace cluster {
namespace {

int32_t StatusCode(const Status& s) {
  if (s.ok()) return 0;
  if (s.IsNotFound()) return 1;
  return 2;
}

Status DecodeRemoteStatus(int32_t code, const std::string& message) {
  if (code == 0) return Status::OK();
  if (code == 1) return Status::NotFound(message);
  return Status::IOError(message);
}

}  // namespace

Status KvRpcService::Handle(const std::string& request_payload,
                            std::string* response_payload) {
  if (cluster_ == nullptr || response_payload == nullptr) {
    return Status::InvalidArgument("KV service is not initialized");
  }
  rpc::RpcHeader header;
  std::string body;
  Status s = rpc::DecodePayload(request_payload, &header, &body);
  if (!s.ok()) return s;
  rpc::RpcHeader response_header = header;
  std::string response_body;

  if (header.type == rpc::MsgType::kKvPutReq) {
    rpc::KvPutReq req;
    s = rpc::DecodeKvPutReq(body, &req);
    if (!s.ok()) return s;
    Status operation = cluster_->Put(req.key, req.value);
    rpc::KvPutResp resp;
    resp.code = StatusCode(operation);
    resp.message = operation.ok() ? "" : operation.ToString();
    s = rpc::EncodeKvPutResp(resp, &response_body);
    response_header.type = rpc::MsgType::kKvPutResp;
  } else if (header.type == rpc::MsgType::kKvGetReq) {
    rpc::KvGetReq req;
    s = rpc::DecodeKvGetReq(body, &req);
    if (!s.ok()) return s;
    rpc::KvGetResp resp;
    Status operation = cluster_->Get(req.key, &resp.value);
    resp.code = StatusCode(operation);
    resp.message = operation.ok() ? "" : operation.ToString();
    s = rpc::EncodeKvGetResp(resp, &response_body);
    response_header.type = rpc::MsgType::kKvGetResp;
  } else if (header.type == rpc::MsgType::kKvDeleteReq) {
    rpc::KvDeleteReq req;
    s = rpc::DecodeKvDeleteReq(body, &req);
    if (!s.ok()) return s;
    Status operation = cluster_->Delete(req.key);
    rpc::KvDeleteResp resp;
    resp.code = StatusCode(operation);
    resp.message = operation.ok() ? "" : operation.ToString();
    s = rpc::EncodeKvDeleteResp(resp, &response_body);
    response_header.type = rpc::MsgType::kKvDeleteResp;
  } else {
    return Status::InvalidArgument("unsupported KV RPC message");
  }
  if (!s.ok()) return s;
  return rpc::EncodePayload(response_header, response_body, response_payload);
}

Status RaftKvRpcService::Handle(const std::string& request_payload,
                                std::string* response_payload) {
  if (node_ == nullptr || db_ == nullptr || response_payload == nullptr) {
    return Status::InvalidArgument("Raft KV service is not initialized");
  }
  rpc::RpcHeader header;
  std::string body;
  Status s = rpc::DecodePayload(request_payload, &header, &body);
  if (!s.ok()) return s;
  if (header.type == rpc::MsgType::kRaftRequestVoteReq ||
      header.type == rpc::MsgType::kRaftAppendEntriesReq ||
      header.type == rpc::MsgType::kRaftInstallSnapshotReq) {
    return raft::HandleRaftRpcPayload(node_, request_payload, response_payload);
  }

  rpc::RpcHeader response_header = header;
  std::string response_body;
  if (header.type == rpc::MsgType::kKvPutReq) {
    rpc::KvPutReq req;
    s = rpc::DecodeKvPutReq(body, &req);
    if (!s.ok()) return s;
    Status operation = node_->Propose(
        raft::KvCommand::EncodePut(req.key, req.value), nullptr);
    rpc::KvPutResp resp{StatusCode(operation),
                        operation.ok() ? "" : operation.ToString()};
    s = rpc::EncodeKvPutResp(resp, &response_body);
    response_header.type = rpc::MsgType::kKvPutResp;
  } else if (header.type == rpc::MsgType::kKvGetReq) {
    rpc::KvGetReq req;
    s = rpc::DecodeKvGetReq(body, &req);
    if (!s.ok()) return s;
    rpc::KvGetResp resp;
    Status operation = node_->ConfirmLeadership();
    if (operation.ok()) operation = db_->Get(ReadOptions(), req.key, &resp.value);
    resp.code = StatusCode(operation);
    resp.message = operation.ok() ? "" : operation.ToString();
    s = rpc::EncodeKvGetResp(resp, &response_body);
    response_header.type = rpc::MsgType::kKvGetResp;
  } else if (header.type == rpc::MsgType::kKvDeleteReq) {
    rpc::KvDeleteReq req;
    s = rpc::DecodeKvDeleteReq(body, &req);
    if (!s.ok()) return s;
    Status operation = node_->Propose(raft::KvCommand::EncodeDelete(req.key),
                                      nullptr);
    rpc::KvDeleteResp resp{StatusCode(operation),
                           operation.ok() ? "" : operation.ToString()};
    s = rpc::EncodeKvDeleteResp(resp, &response_body);
    response_header.type = rpc::MsgType::kKvDeleteResp;
  } else {
    return Status::InvalidArgument("unsupported node RPC message");
  }
  if (!s.ok()) return s;
  return rpc::EncodePayload(response_header, response_body, response_payload);
}

RemoteClusterClient::RemoteClusterClient(std::string host, uint16_t port)
    : host_(std::move(host)), port_(port) {}

Status RemoteClusterClient::Call(rpc::MsgType request_type,
                                 rpc::MsgType response_type,
                                 const std::string& request_body,
                                 std::string* response_body) {
  rpc::RpcClient client;
  Status s = client.Connect(host_, port_);
  if (!s.ok()) return s;
  rpc::RpcHeader header;
  header.type = request_type;
  header.request_id = next_request_id_.fetch_add(1);
  std::string request_payload;
  s = rpc::EncodePayload(header, request_body, &request_payload);
  if (!s.ok()) return s;
  std::string response_payload;
  s = client.Call(request_payload, &response_payload);
  if (!s.ok()) return s;
  rpc::RpcHeader response_header;
  s = rpc::DecodePayload(response_payload, &response_header, response_body);
  if (!s.ok()) return s;
  if (response_header.type != response_type ||
      response_header.request_id != header.request_id) {
    return Status::Corruption("unexpected KV RPC response");
  }
  return Status::OK();
}

Status RemoteClusterClient::Put(const std::string& key,
                                const std::string& value) {
  rpc::KvPutReq req{key, value};
  std::string body;
  Status s = rpc::EncodeKvPutReq(req, &body);
  if (!s.ok()) return s;
  std::string response_body;
  s = Call(rpc::MsgType::kKvPutReq, rpc::MsgType::kKvPutResp, body,
           &response_body);
  if (!s.ok()) return s;
  rpc::KvPutResp resp;
  s = rpc::DecodeKvPutResp(response_body, &resp);
  return s.ok() ? DecodeRemoteStatus(resp.code, resp.message) : s;
}

Status RemoteClusterClient::Get(const std::string& key, std::string* value) {
  if (value == nullptr) return Status::InvalidArgument("null value output");
  rpc::KvGetReq req{key};
  std::string body;
  Status s = rpc::EncodeKvGetReq(req, &body);
  if (!s.ok()) return s;
  std::string response_body;
  s = Call(rpc::MsgType::kKvGetReq, rpc::MsgType::kKvGetResp, body,
           &response_body);
  if (!s.ok()) return s;
  rpc::KvGetResp resp;
  s = rpc::DecodeKvGetResp(response_body, &resp);
  if (!s.ok()) return s;
  s = DecodeRemoteStatus(resp.code, resp.message);
  if (s.ok()) *value = std::move(resp.value);
  return s;
}

Status RemoteClusterClient::Delete(const std::string& key) {
  rpc::KvDeleteReq req{key};
  std::string body;
  Status s = rpc::EncodeKvDeleteReq(req, &body);
  if (!s.ok()) return s;
  std::string response_body;
  s = Call(rpc::MsgType::kKvDeleteReq, rpc::MsgType::kKvDeleteResp, body,
           &response_body);
  if (!s.ok()) return s;
  rpc::KvDeleteResp resp;
  s = rpc::DecodeKvDeleteResp(response_body, &resp);
  return s.ok() ? DecodeRemoteStatus(resp.code, resp.message) : s;
}

}  // namespace cluster
}  // namespace minikv
