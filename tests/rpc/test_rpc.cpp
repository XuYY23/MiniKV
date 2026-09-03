#include "rpc/client.h"
#include "rpc/codec.h"
#include "rpc/message.h"
#include "rpc/server.h"
#include "tests/testharness.h"

#include <chrono>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace {

minikv::Status DefaultEchoHandler(const std::string& req_payload,
                                  std::string* resp_payload) {
  minikv::rpc::RpcHeader header;
  std::string body;
  minikv::Status s = minikv::rpc::DecodePayload(req_payload, &header, &body);
  if (!s.ok()) {
    return s;
  }

  if (header.type == minikv::rpc::MsgType::kPing) {
    minikv::rpc::Ping ping;
    s = minikv::rpc::DecodePing(body, &ping);
    if (!s.ok()) {
      return s;
    }
    minikv::rpc::Pong pong;
    pong.nonce = ping.nonce;
    std::string out_body;
    s = minikv::rpc::EncodePong(pong, &out_body);
    if (!s.ok()) {
      return s;
    }
    minikv::rpc::RpcHeader rh = header;
    rh.type = minikv::rpc::MsgType::kPong;
    return minikv::rpc::EncodePayload(rh, out_body, resp_payload);
  }

  if (header.type == minikv::rpc::MsgType::kKvPutReq) {
    minikv::rpc::KvPutReq req;
    s = minikv::rpc::DecodeKvPutReq(body, &req);
    if (!s.ok()) {
      return s;
    }
    minikv::rpc::KvPutResp resp;
    resp.code = 0;
    resp.message = "ok:" + req.key;
    std::string out_body;
    s = minikv::rpc::EncodeKvPutResp(resp, &out_body);
    if (!s.ok()) {
      return s;
    }
    minikv::rpc::RpcHeader rh = header;
    rh.type = minikv::rpc::MsgType::kKvPutResp;
    return minikv::rpc::EncodePayload(rh, out_body, resp_payload);
  }

  return minikv::Status::InvalidArgument("unsupported msg type in test handler");
}

}  // namespace

int main() {
  minikv::test::LogSection("TEST SUITE: test_rpc (小节 D1 · RPC 协议)");

  minikv::test::LogStep("codec: Ping payload encode/decode roundtrip");
  {
    minikv::rpc::Ping ping;
    ping.nonce = "hello-rpc";
    std::string body;
    CHECK_OK(minikv::rpc::EncodePing(ping, &body));
    minikv::rpc::Ping out;
    CHECK_OK(minikv::rpc::DecodePing(body, &out));
    CHECK_EQ(out.nonce, std::string("hello-rpc"));

    minikv::rpc::RpcHeader header;
    header.type = minikv::rpc::MsgType::kPing;
    header.request_id = 42;
    std::string payload;
    CHECK_OK(minikv::rpc::EncodePayload(header, body, &payload));
    std::string frame;
    CHECK_OK(minikv::rpc::EncodeFrame(payload, &frame));

    size_t consumed = 0;
    std::string payload2;
    CHECK_OK(minikv::rpc::TryDecodeFrame(frame, &consumed, &payload2));
    CHECK_EQ(consumed, frame.size());
    CHECK_EQ(payload2, payload);

    minikv::test::LogStep("codec: truncated buffer should not consume");
    consumed = 0;
    payload2.clear();
    CHECK_OK(minikv::rpc::TryDecodeFrame(frame.substr(0, 3), &consumed, &payload2));
    CHECK_EQ(consumed, static_cast<size_t>(0));

    minikv::test::LogStep("codec: bad magic rejected");
    std::string bad = payload;
    bad[0] = static_cast<char>(bad[0] ^ 0xff);
    minikv::rpc::RpcHeader h2;
    std::string b2;
    CHECK(!minikv::rpc::DecodePayload(bad, &h2, &b2).ok());
  }

  minikv::test::LogStep("codec: KV Put / Raft RequestVote messages");
  {
    minikv::rpc::KvPutReq put;
    put.key = "k";
    put.value = "v";
    std::string body;
    CHECK_OK(minikv::rpc::EncodeKvPutReq(put, &body));
    minikv::rpc::KvPutReq put2;
    CHECK_OK(minikv::rpc::DecodeKvPutReq(body, &put2));
    CHECK_EQ(put2.key, std::string("k"));
    CHECK_EQ(put2.value, std::string("v"));

    minikv::rpc::RaftRequestVoteReq vote;
    vote.term = 3;
    vote.candidate_id = 2;
    vote.last_log_index = 10;
    vote.last_log_term = 2;
    CHECK_OK(minikv::rpc::EncodeRaftRequestVoteReq(vote, &body));
    minikv::rpc::RaftRequestVoteReq vote2;
    CHECK_OK(minikv::rpc::DecodeRaftRequestVoteReq(body, &vote2));
    CHECK_EQ(vote2.term, static_cast<uint64_t>(3));
    CHECK_EQ(vote2.candidate_id, static_cast<uint64_t>(2));

    minikv::rpc::RaftAppendEntriesReq ae;
    ae.term = 5;
    ae.leader_id = 1;
    ae.prev_log_index = 9;
    ae.prev_log_term = 4;
    ae.leader_commit = 8;
    minikv::rpc::RaftLogEntry e;
    e.term = 5;
    e.command = "PUT x y";
    ae.entries.push_back(e);
    CHECK_OK(minikv::rpc::EncodeRaftAppendEntriesReq(ae, &body));
    minikv::rpc::RaftAppendEntriesReq ae2;
    CHECK_OK(minikv::rpc::DecodeRaftAppendEntriesReq(body, &ae2));
    CHECK_EQ(ae2.entries.size(), static_cast<size_t>(1));
    CHECK_EQ(ae2.entries[0].command, std::string("PUT x y"));
    std::string impossible_count = body;
    impossible_count[40] = static_cast<char>(0xff);
    impossible_count[41] = static_cast<char>(0xff);
    impossible_count[42] = static_cast<char>(0xff);
    impossible_count[43] = static_cast<char>(0xff);
    CHECK(!minikv::rpc::DecodeRaftAppendEntriesReq(impossible_count, &ae2).ok());

    minikv::rpc::RaftInstallSnapshotReq snapshot;
    snapshot.term = 6;
    snapshot.leader_id = 1;
    snapshot.last_included_index = 12;
    snapshot.last_included_term = 5;
    snapshot.data = std::string("state\0bytes", 11);
    CHECK_OK(minikv::rpc::EncodeRaftInstallSnapshotReq(snapshot, &body));
    minikv::rpc::RaftInstallSnapshotReq snapshot2;
    CHECK_OK(minikv::rpc::DecodeRaftInstallSnapshotReq(body, &snapshot2));
    CHECK_EQ(snapshot2.last_included_index, static_cast<uint64_t>(12));
    CHECK_EQ(snapshot2.data, snapshot.data);
  }

  minikv::test::LogStep("start RpcServer on 127.0.0.1:0 (ephemeral port)");
  minikv::rpc::RpcServer server;
  server.SetHandler(DefaultEchoHandler);
  CHECK_OK(server.Start("127.0.0.1", 0));
  const uint16_t port = server.BoundPort();
  CHECK(port > 0);
  minikv::test::Log("  bound_port=" + std::to_string(port));

  // 给 accept 线程一点启动时间。
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  minikv::test::LogStep("RpcClient Ping -> Pong over TCP");
  {
    minikv::rpc::RpcClient client;
    CHECK_OK(client.Connect("127.0.0.1", port));
    std::string echoed;
    CHECK_OK(client.CallPing("mini-kv-d1", &echoed, /*request_id=*/7));
    CHECK_EQ(echoed, std::string("mini-kv-d1"));
    client.Close();
  }

  minikv::test::LogStep("concurrent TCP clients preserve request/response IDs");
  {
    std::atomic<int> failures{0};
    std::vector<std::thread> clients;
    for (int i = 0; i < 8; ++i) {
      clients.emplace_back([&, i] {
        minikv::rpc::RpcClient client;
        std::string echoed;
        const std::string nonce = "parallel-" + std::to_string(i);
        if (!client.Connect("127.0.0.1", port).ok() ||
            !client.CallPing(nonce, &echoed, 100 + i).ok() || echoed != nonce) {
          ++failures;
        }
      });
    }
    for (auto& client : clients) client.join();
    CHECK_EQ(failures.load(), 0);
  }

  minikv::test::LogStep("RpcClient KvPutReq roundtrip");
  {
    minikv::rpc::RpcClient client;
    CHECK_OK(client.Connect("127.0.0.1", port));

    minikv::rpc::KvPutReq put;
    put.key = "user:1";
    put.value = "alice";
    std::string body;
    CHECK_OK(minikv::rpc::EncodeKvPutReq(put, &body));
    minikv::rpc::RpcHeader header;
    header.type = minikv::rpc::MsgType::kKvPutReq;
    header.request_id = 99;
    std::string payload;
    CHECK_OK(minikv::rpc::EncodePayload(header, body, &payload));

    std::string resp_payload;
    CHECK_OK(client.Call(payload, &resp_payload));
    minikv::rpc::RpcHeader rh;
    std::string rbody;
    CHECK_OK(minikv::rpc::DecodePayload(resp_payload, &rh, &rbody));
    CHECK(rh.type == minikv::rpc::MsgType::kKvPutResp);
    CHECK_EQ(rh.request_id, static_cast<uint64_t>(99));
    minikv::rpc::KvPutResp resp;
    CHECK_OK(minikv::rpc::DecodeKvPutResp(rbody, &resp));
    CHECK_EQ(resp.code, 0);
    CHECK_EQ(resp.message, std::string("ok:user:1"));
    client.Close();
  }

  minikv::test::LogStep("stop server");
  server.Stop();
  CHECK(!server.Running());

  return minikv::test::Report("test_rpc");
}
