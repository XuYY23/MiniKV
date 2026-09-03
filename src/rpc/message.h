#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace minikv {
namespace rpc {

// 帧： [len:u32 LE][payload]
// payload： [magic:u32][version:u16][type:u16][request_id:u64][body...]
constexpr uint32_t kRpcMagic = 0x314b564d;  // 'MVK1' LE
constexpr uint16_t kRpcVersion = 1;
constexpr size_t kRpcHeaderSize = 4 + 2 + 2 + 8;  // 16
constexpr size_t kRpcMaxPayload = 16 * 1024 * 1024;

enum class MsgType : uint16_t {
  kPing = 1,
  kPong = 2,

  kKvPutReq = 10,
  kKvPutResp = 11,
  kKvGetReq = 12,
  kKvGetResp = 13,
  kKvDeleteReq = 14,
  kKvDeleteResp = 15,

  // Raft
  kRaftRequestVoteReq = 20,
  kRaftRequestVoteResp = 21,
  kRaftAppendEntriesReq = 22,
  kRaftAppendEntriesResp = 23,
  kRaftInstallSnapshotReq = 24,
  kRaftInstallSnapshotResp = 25,
};

struct RpcHeader {
  uint32_t magic = kRpcMagic;
  uint16_t version = kRpcVersion;
  MsgType type = MsgType::kPing;
  uint64_t request_id = 0;
};

struct Ping {
  std::string nonce;
};

struct Pong {
  std::string nonce;
};

struct KvPutReq {
  std::string key;
  std::string value;
};

struct KvPutResp {
  int32_t code = 0;  // 0=OK
  std::string message;
};

struct KvGetReq {
  std::string key;
};

struct KvGetResp {
  int32_t code = 0;  // 0=OK, 1=NotFound
  std::string value;
  std::string message;
};

struct KvDeleteReq {
  std::string key;
};

struct KvDeleteResp {
  int32_t code = 0;
  std::string message;
};

struct RaftRequestVoteReq {
  uint64_t term = 0;
  uint64_t candidate_id = 0;
  uint64_t last_log_index = 0;
  uint64_t last_log_term = 0;
};

struct RaftRequestVoteResp {
  uint64_t term = 0;
  uint8_t vote_granted = 0;
};

struct RaftLogEntry {
  uint64_t term = 0;
  std::string command;
};

struct RaftAppendEntriesReq {
  uint64_t term = 0;
  uint64_t leader_id = 0;
  uint64_t prev_log_index = 0;
  uint64_t prev_log_term = 0;
  uint64_t leader_commit = 0;
  std::vector<RaftLogEntry> entries;
};

struct RaftAppendEntriesResp {
  uint64_t term = 0;
  uint8_t success = 0;
};

struct RaftInstallSnapshotReq {
  uint64_t term = 0;
  uint64_t leader_id = 0;
  uint64_t last_included_index = 0;
  uint64_t last_included_term = 0;
  std::string data;
};

struct RaftInstallSnapshotResp {
  uint64_t term = 0;
  uint8_t success = 0;
};

}  // namespace rpc
}  // namespace minikv
