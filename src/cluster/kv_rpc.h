#pragma once

#include "cluster/shard_cluster.h"
#include "minikv/status.h"
#include "raft/node.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace minikv {
namespace cluster {

// Adapts the cluster API to the KV messages defined by the RPC protocol.
class KvRpcService {
 public:
  explicit KvRpcService(ShardCluster* cluster) : cluster_(cluster) {}
  Status Handle(const std::string& request_payload,
                std::string* response_payload);

 private:
  ShardCluster* cluster_;
};

// RPC facade for one independently deployed Raft replica and its local DB.
// The same listener accepts both Raft peer traffic and client KV traffic.
class RaftKvRpcService {
 public:
  RaftKvRpcService(raft::RaftNode* node, DB* db) : node_(node), db_(db) {}
  Status Handle(const std::string& request_payload,
                std::string* response_payload);

 private:
  raft::RaftNode* node_;
  DB* db_;
};

// Network client for Put/Get/Delete. Routing remains server-side so topology
// changes do not require rebuilding the client.
class RemoteClusterClient {
 public:
  RemoteClusterClient(std::string host, uint16_t port);

  Status Put(const std::string& key, const std::string& value);
  Status Get(const std::string& key, std::string* value);
  Status Delete(const std::string& key);

 private:
  Status Call(rpc::MsgType request_type, rpc::MsgType response_type,
              const std::string& request_body, std::string* response_body);

  std::string host_;
  uint16_t port_;
  std::atomic<uint64_t> next_request_id_{1};
};

}  // namespace cluster
}  // namespace minikv
