#pragma once

#include <cstdint>
#include <string>

namespace minikv {
namespace raft {

using NodeId = uint64_t;
using Term = uint64_t;
using LogIndex = uint64_t;

enum class Role {
  kFollower = 0,
  kCandidate = 1,
  kLeader = 2,
};

inline const char* RoleName(Role r) {
  switch (r) {
    case Role::kFollower:
      return "Follower";
    case Role::kCandidate:
      return "Candidate";
    case Role::kLeader:
      return "Leader";
  }
  return "?";
}

struct LogEntry {
  Term term = 0;
  std::string command;
};

struct Snapshot {
  LogIndex last_included_index = 0;
  Term last_included_term = 0;
  std::string data;
};

}  // namespace raft
}  // namespace minikv
