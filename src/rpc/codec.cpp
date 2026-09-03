#include "rpc/codec.h"

#include "engine/coding.h"
#include "minikv/slice.h"

#include <cstring>

namespace minikv {
namespace rpc {
namespace {

void PutU16(std::string* dst, uint16_t v) {
  char buf[2] = {static_cast<char>(v & 0xff),
                 static_cast<char>((v >> 8) & 0xff)};
  dst->append(buf, sizeof(buf));
}

uint16_t DecodeU16(const char* p) {
  return static_cast<uint16_t>(static_cast<unsigned char>(p[0])) |
         static_cast<uint16_t>(static_cast<unsigned char>(p[1]) << 8);
}

Status EncodeString(const std::string& s, std::string* out) {
  PutLengthPrefixedSlice(out, s);
  return Status::OK();
}

Status DecodeString(Slice* in, std::string* out) {
  Slice s;
  if (!GetLengthPrefixedSlice(in, &s)) {
    return Status::Corruption("bad length-prefixed string");
  }
  out->assign(s.data(), s.size());
  return Status::OK();
}

}  // namespace

Status EncodePayload(const RpcHeader& header, const std::string& body,
                     std::string* payload) {
  if (payload == nullptr) {
    return Status::InvalidArgument("null payload");
  }
  payload->clear();
  payload->reserve(kRpcHeaderSize + body.size());
  PutFixed32(payload, header.magic);
  PutU16(payload, header.version);
  PutU16(payload, static_cast<uint16_t>(header.type));
  PutFixed64(payload, header.request_id);
  payload->append(body);
  return Status::OK();
}

Status DecodePayload(const std::string& payload, RpcHeader* header,
                     std::string* body) {
  if (header == nullptr || body == nullptr) {
    return Status::InvalidArgument("null out");
  }
  if (payload.size() < kRpcHeaderSize) {
    return Status::Corruption("payload shorter than header");
  }
  header->magic = DecodeFixed32(payload.data());
  header->version = DecodeU16(payload.data() + 4);
  header->type = static_cast<MsgType>(DecodeU16(payload.data() + 6));
  header->request_id = DecodeFixed64(payload.data() + 8);
  if (header->magic != kRpcMagic) {
    return Status::Corruption("bad rpc magic");
  }
  if (header->version != kRpcVersion) {
    return Status::Corruption("unsupported rpc version");
  }
  body->assign(payload.data() + kRpcHeaderSize,
               payload.size() - kRpcHeaderSize);
  return Status::OK();
}

Status EncodeFrame(const std::string& payload, std::string* frame) {
  if (frame == nullptr) {
    return Status::InvalidArgument("null frame");
  }
  if (payload.size() > kRpcMaxPayload) {
    return Status::InvalidArgument("payload too large");
  }
  frame->clear();
  PutFixed32(frame, static_cast<uint32_t>(payload.size()));
  frame->append(payload);
  return Status::OK();
}

Status TryDecodeFrame(const std::string& buffer, size_t* consumed,
                      std::string* payload) {
  if (consumed == nullptr || payload == nullptr) {
    return Status::InvalidArgument("null out");
  }
  *consumed = 0;
  payload->clear();
  if (buffer.size() < 4) {
    return Status::OK();
  }
  const uint32_t len = DecodeFixed32(buffer.data());
  if (len > kRpcMaxPayload) {
    return Status::Corruption("frame length too large");
  }
  if (buffer.size() < 4 + static_cast<size_t>(len)) {
    return Status::OK();
  }
  payload->assign(buffer.data() + 4, len);
  *consumed = 4 + static_cast<size_t>(len);
  return Status::OK();
}

Status EncodePing(const Ping& m, std::string* body) {
  body->clear();
  return EncodeString(m.nonce, body);
}
Status DecodePing(const std::string& body, Ping* m) {
  Slice in(body);
  return DecodeString(&in, &m->nonce);
}
Status EncodePong(const Pong& m, std::string* body) {
  body->clear();
  return EncodeString(m.nonce, body);
}
Status DecodePong(const std::string& body, Pong* m) {
  Slice in(body);
  return DecodeString(&in, &m->nonce);
}

Status EncodeKvPutReq(const KvPutReq& m, std::string* body) {
  body->clear();
  Status s = EncodeString(m.key, body);
  if (!s.ok()) {
    return s;
  }
  return EncodeString(m.value, body);
}
Status DecodeKvPutReq(const std::string& body, KvPutReq* m) {
  Slice in(body);
  Status s = DecodeString(&in, &m->key);
  if (!s.ok()) {
    return s;
  }
  return DecodeString(&in, &m->value);
}
Status EncodeKvPutResp(const KvPutResp& m, std::string* body) {
  body->clear();
  PutFixed32(body, static_cast<uint32_t>(m.code));
  return EncodeString(m.message, body);
}
Status DecodeKvPutResp(const std::string& body, KvPutResp* m) {
  if (body.size() < 4) {
    return Status::Corruption("short KvPutResp");
  }
  m->code = static_cast<int32_t>(DecodeFixed32(body.data()));
  Slice in(body.data() + 4, body.size() - 4);
  return DecodeString(&in, &m->message);
}

Status EncodeKvGetReq(const KvGetReq& m, std::string* body) {
  body->clear();
  return EncodeString(m.key, body);
}
Status DecodeKvGetReq(const std::string& body, KvGetReq* m) {
  Slice in(body);
  return DecodeString(&in, &m->key);
}
Status EncodeKvGetResp(const KvGetResp& m, std::string* body) {
  body->clear();
  PutFixed32(body, static_cast<uint32_t>(m.code));
  Status s = EncodeString(m.value, body);
  if (!s.ok()) {
    return s;
  }
  return EncodeString(m.message, body);
}
Status DecodeKvGetResp(const std::string& body, KvGetResp* m) {
  if (body.size() < 4) {
    return Status::Corruption("short KvGetResp");
  }
  m->code = static_cast<int32_t>(DecodeFixed32(body.data()));
  Slice in(body.data() + 4, body.size() - 4);
  Status s = DecodeString(&in, &m->value);
  if (!s.ok()) {
    return s;
  }
  return DecodeString(&in, &m->message);
}

Status EncodeKvDeleteReq(const KvDeleteReq& m, std::string* body) {
  body->clear();
  return EncodeString(m.key, body);
}
Status DecodeKvDeleteReq(const std::string& body, KvDeleteReq* m) {
  Slice in(body);
  return DecodeString(&in, &m->key);
}
Status EncodeKvDeleteResp(const KvDeleteResp& m, std::string* body) {
  body->clear();
  PutFixed32(body, static_cast<uint32_t>(m.code));
  return EncodeString(m.message, body);
}
Status DecodeKvDeleteResp(const std::string& body, KvDeleteResp* m) {
  if (body.size() < 4) {
    return Status::Corruption("short KvDeleteResp");
  }
  m->code = static_cast<int32_t>(DecodeFixed32(body.data()));
  Slice in(body.data() + 4, body.size() - 4);
  return DecodeString(&in, &m->message);
}

Status EncodeRaftRequestVoteReq(const RaftRequestVoteReq& m, std::string* body) {
  body->clear();
  PutFixed64(body, m.term);
  PutFixed64(body, m.candidate_id);
  PutFixed64(body, m.last_log_index);
  PutFixed64(body, m.last_log_term);
  return Status::OK();
}
Status DecodeRaftRequestVoteReq(const std::string& body, RaftRequestVoteReq* m) {
  if (body.size() < 32) {
    return Status::Corruption("short RequestVoteReq");
  }
  m->term = DecodeFixed64(body.data());
  m->candidate_id = DecodeFixed64(body.data() + 8);
  m->last_log_index = DecodeFixed64(body.data() + 16);
  m->last_log_term = DecodeFixed64(body.data() + 24);
  return Status::OK();
}
Status EncodeRaftRequestVoteResp(const RaftRequestVoteResp& m,
                                 std::string* body) {
  body->clear();
  PutFixed64(body, m.term);
  body->push_back(static_cast<char>(m.vote_granted));
  return Status::OK();
}
Status DecodeRaftRequestVoteResp(const std::string& body,
                                 RaftRequestVoteResp* m) {
  if (body.size() < 9) {
    return Status::Corruption("short RequestVoteResp");
  }
  m->term = DecodeFixed64(body.data());
  m->vote_granted = static_cast<uint8_t>(body[8]);
  return Status::OK();
}

Status EncodeRaftAppendEntriesReq(const RaftAppendEntriesReq& m,
                                  std::string* body) {
  body->clear();
  PutFixed64(body, m.term);
  PutFixed64(body, m.leader_id);
  PutFixed64(body, m.prev_log_index);
  PutFixed64(body, m.prev_log_term);
  PutFixed64(body, m.leader_commit);
  PutFixed32(body, static_cast<uint32_t>(m.entries.size()));
  for (const auto& e : m.entries) {
    PutFixed64(body, e.term);
    Status s = EncodeString(e.command, body);
    if (!s.ok()) {
      return s;
    }
  }
  return Status::OK();
}
Status DecodeRaftAppendEntriesReq(const std::string& body,
                                  RaftAppendEntriesReq* m) {
  if (body.size() < 44) {
    return Status::Corruption("short AppendEntriesReq");
  }
  m->term = DecodeFixed64(body.data());
  m->leader_id = DecodeFixed64(body.data() + 8);
  m->prev_log_index = DecodeFixed64(body.data() + 16);
  m->prev_log_term = DecodeFixed64(body.data() + 24);
  m->leader_commit = DecodeFixed64(body.data() + 32);
  const uint32_t n = DecodeFixed32(body.data() + 40);
  Slice in(body.data() + 44, body.size() - 44);
  if (n > in.size() / 9) {
    return Status::Corruption("impossible AppendEntries count");
  }
  m->entries.clear();
  m->entries.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    if (in.size() < 8) {
      return Status::Corruption("bad AppendEntries entry");
    }
    RaftLogEntry e;
    e.term = DecodeFixed64(in.data());
    in.remove_prefix(8);
    Status s = DecodeString(&in, &e.command);
    if (!s.ok()) {
      return s;
    }
    m->entries.push_back(std::move(e));
  }
  return Status::OK();
}
Status EncodeRaftAppendEntriesResp(const RaftAppendEntriesResp& m,
                                   std::string* body) {
  body->clear();
  PutFixed64(body, m.term);
  body->push_back(static_cast<char>(m.success));
  return Status::OK();
}
Status DecodeRaftAppendEntriesResp(const std::string& body,
                                   RaftAppendEntriesResp* m) {
  if (body.size() < 9) {
    return Status::Corruption("short AppendEntriesResp");
  }
  m->term = DecodeFixed64(body.data());
  m->success = static_cast<uint8_t>(body[8]);
  return Status::OK();
}

Status EncodeRaftInstallSnapshotReq(const RaftInstallSnapshotReq& m,
                                    std::string* body) {
  body->clear();
  PutFixed64(body, m.term);
  PutFixed64(body, m.leader_id);
  PutFixed64(body, m.last_included_index);
  PutFixed64(body, m.last_included_term);
  return EncodeString(m.data, body);
}

Status DecodeRaftInstallSnapshotReq(const std::string& body,
                                    RaftInstallSnapshotReq* m) {
  if (body.size() < 36) return Status::Corruption("short InstallSnapshotReq");
  m->term = DecodeFixed64(body.data());
  m->leader_id = DecodeFixed64(body.data() + 8);
  m->last_included_index = DecodeFixed64(body.data() + 16);
  m->last_included_term = DecodeFixed64(body.data() + 24);
  Slice in(body.data() + 32, body.size() - 32);
  Status s = DecodeString(&in, &m->data);
  if (!s.ok()) return s;
  return in.empty() ? Status::OK()
                    : Status::Corruption("trailing InstallSnapshotReq bytes");
}

Status EncodeRaftInstallSnapshotResp(const RaftInstallSnapshotResp& m,
                                     std::string* body) {
  body->clear();
  PutFixed64(body, m.term);
  body->push_back(static_cast<char>(m.success));
  return Status::OK();
}

Status DecodeRaftInstallSnapshotResp(const std::string& body,
                                     RaftInstallSnapshotResp* m) {
  if (body.size() != 9) return Status::Corruption("bad InstallSnapshotResp");
  m->term = DecodeFixed64(body.data());
  m->success = static_cast<uint8_t>(body[8]);
  return Status::OK();
}

}  // namespace rpc
}  // namespace minikv
