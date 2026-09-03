#include "cluster/cluster_client.h"

#include <chrono>
#include <thread>

namespace minikv {
namespace cluster {

ClusterClient::ClusterClient() = default;

ClusterClient::~ClusterClient() { Close(); }

Status ClusterClient::Open(const ShardClusterOptions& opt) {
  Close();
  cluster_ = std::make_unique<ShardCluster>(opt);
  Status s = cluster_->Open();
  if (!s.ok()) {
    cluster_.reset();
    return s;
  }
  // 先选主，再启后台 Tick（否则 CLI 首条 Put 会 no leader）。
  logical_now_ms_ = 100;
  cluster_->ElectReplica0Leaders(logical_now_ms_.load());
  running_ = true;
  if (auto_tick_) {
    tick_thread_ = std::thread([this] { TickLoop(); });
  }
  return Status::OK();
}

void ClusterClient::Close() {
  running_ = false;
  if (tick_thread_.joinable()) {
    tick_thread_.join();
  }
  if (cluster_) {
    cluster_->Close();
    cluster_.reset();
  }
}

void ClusterClient::TickLoop() {
  while (running_.load()) {
    if (auto_tick_.load()) {
      const uint64_t now = logical_now_ms_.fetch_add(50) + 50;
      cluster_->TickAll(now);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

Status ClusterClient::Put(const std::string& key, const std::string& value) {
  if (!cluster_) {
    return Status::InvalidArgument("client not open");
  }
  return cluster_->Put(key, value);
}

Status ClusterClient::Get(const std::string& key, std::string* value) {
  if (!cluster_) {
    return Status::InvalidArgument("client not open");
  }
  return cluster_->Get(key, value);
}

Status ClusterClient::Delete(const std::string& key) {
  if (!cluster_) {
    return Status::InvalidArgument("client not open");
  }
  return cluster_->Delete(key);
}

ShardId ClusterClient::ShardOfKey(const std::string& key) const {
  if (!cluster_) {
    return 0;
  }
  return cluster_->ShardOfKey(key);
}

ClusterTopology ClusterClient::Topology() const {
  if (!cluster_) {
    return {};
  }
  return cluster_->Topology();
}

void ClusterClient::SetAutoTick(bool on) { auto_tick_ = on; }

void ClusterClient::TickForTest(uint64_t now_ms) {
  logical_now_ms_ = now_ms;
  if (cluster_) {
    cluster_->TickAll(now_ms);
  }
}

void ClusterClient::ElectLeadersForTest(uint64_t elect_at_ms) {
  logical_now_ms_ = elect_at_ms;
  if (cluster_) {
    cluster_->ElectReplica0Leaders(elect_at_ms);
  }
}

}  // namespace cluster
}  // namespace minikv
