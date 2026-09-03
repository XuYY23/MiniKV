#pragma once

#include "minikv/status.h"
#include "rpc/message.h"

#include <string>

namespace minikv {
namespace rpc {

// 把 header + body 拼成 payload（不含外层 len）。
Status EncodePayload(const RpcHeader& header, const std::string& body,
                     std::string* payload);

// 解析 payload 头部；body 指向 header 之后的剩余字节（拷贝到 *body）。
Status DecodePayload(const std::string& payload, RpcHeader* header,
                     std::string* body);

// 外层帧：len + payload
Status EncodeFrame(const std::string& payload, std::string* frame);
// 从缓冲中尝试拆出一帧；返回消耗字节数，不足返回 0，损坏返回错误。
Status TryDecodeFrame(const std::string& buffer, size_t* consumed,
                      std::string* payload);

Status EncodePing(const Ping& m, std::string* body);
Status DecodePing(const std::string& body, Ping* m);
Status EncodePong(const Pong& m, std::string* body);
Status DecodePong(const std::string& body, Pong* m);

Status EncodeKvPutReq(const KvPutReq& m, std::string* body);
Status DecodeKvPutReq(const std::string& body, KvPutReq* m);
Status EncodeKvPutResp(const KvPutResp& m, std::string* body);
Status DecodeKvPutResp(const std::string& body, KvPutResp* m);

Status EncodeKvGetReq(const KvGetReq& m, std::string* body);
Status DecodeKvGetReq(const std::string& body, KvGetReq* m);
Status EncodeKvGetResp(const KvGetResp& m, std::string* body);
Status DecodeKvGetResp(const std::string& body, KvGetResp* m);

Status EncodeKvDeleteReq(const KvDeleteReq& m, std::string* body);
Status DecodeKvDeleteReq(const std::string& body, KvDeleteReq* m);
Status EncodeKvDeleteResp(const KvDeleteResp& m, std::string* body);
Status DecodeKvDeleteResp(const std::string& body, KvDeleteResp* m);

Status EncodeRaftRequestVoteReq(const RaftRequestVoteReq& m, std::string* body);
Status DecodeRaftRequestVoteReq(const std::string& body, RaftRequestVoteReq* m);
Status EncodeRaftRequestVoteResp(const RaftRequestVoteResp& m,
                                 std::string* body);
Status DecodeRaftRequestVoteResp(const std::string& body,
                                 RaftRequestVoteResp* m);

Status EncodeRaftAppendEntriesReq(const RaftAppendEntriesReq& m,
                                  std::string* body);
Status DecodeRaftAppendEntriesReq(const std::string& body,
                                  RaftAppendEntriesReq* m);
Status EncodeRaftAppendEntriesResp(const RaftAppendEntriesResp& m,
                                   std::string* body);
Status DecodeRaftAppendEntriesResp(const std::string& body,
                                   RaftAppendEntriesResp* m);
Status EncodeRaftInstallSnapshotReq(const RaftInstallSnapshotReq& m,
                                    std::string* body);
Status DecodeRaftInstallSnapshotReq(const std::string& body,
                                    RaftInstallSnapshotReq* m);
Status EncodeRaftInstallSnapshotResp(const RaftInstallSnapshotResp& m,
                                     std::string* body);
Status DecodeRaftInstallSnapshotResp(const std::string& body,
                                     RaftInstallSnapshotResp* m);

}  // namespace rpc
}  // namespace minikv
