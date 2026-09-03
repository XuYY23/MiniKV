# 07 · RPC 协议与服务

节点间（Raft）与客户端（KV）共用同一套长度前缀 TCP 帧。

## 帧与消息

```text
TCP 字节流
  └─ frame = [len:u32 LE][payload]
       payload = [magic:u32][version:u16][type:u16][request_id:u64][body...]
```

- `Ping` / `Pong`：连通性与编解码通路  
- `KvPut` / `Get` / `Delete` Req/Resp：客户端通道  
- `RaftRequestVote` / `AppendEntries` Req/Resp：节点间通道  

实现目录：`src/rpc/`（`message.h` / `codec.*` / `server.*` / `client.*`）。`KvRpcService` 将 KV 消息连接到分片集群，`RemoteClusterClient` 提供网络客户端 API；`TcpRaftTransport` 负责节点间 Raft 调用。

## 运行测试

```bash
./build/test_rpc
```

覆盖：编解码往返、截断帧、坏 magic、本机 TCP Ping、三节点 TCP Raft 复制，以及远程 KV 的 Put/Get/Delete 全链路。
