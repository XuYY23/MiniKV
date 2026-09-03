#include "bench/bench_common.h"
#include "engine/db_impl.h"
#include "minikv/db.h"
#include "minikv/status.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void Usage(const char* argv0) {
  std::cerr
      << "用法: " << argv0
      << " [--ops N] [--value-bytes B] [--db DIR]\n"
      << "  在相对目录下对 MiniKV / memory_map / simple_wal 做吞吐对比。\n"
      << "  默认: --ops 5000 --value-bytes 100 --db ./data/bench\n";
}

int ParseInt(const char* s, int def) {
  if (s == nullptr || *s == '\0') {
    return def;
  }
  return std::atoi(s);
}

bool EnsureDir(minikv::Env* env, const std::string& path) {
  if (env->FileExists(path)) {
    return true;
  }
  minikv::Status s = env->CreateDir(path);
  return s.ok() || env->FileExists(path);
}

}  // namespace

int main(int argc, char** argv) {
  int ops = 5000;
  int value_bytes = 100;
  std::string dbroot = "./data/bench";

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--ops" && i + 1 < argc) {
      ops = ParseInt(argv[++i], ops);
    } else if (arg == "--value-bytes" && i + 1 < argc) {
      value_bytes = ParseInt(argv[++i], value_bytes);
    } else if (arg == "--db" && i + 1 < argc) {
      dbroot = argv[++i];
    } else if (arg == "-h" || arg == "--help") {
      Usage(argv[0]);
      return 0;
    } else {
      Usage(argv[0]);
      return 1;
    }
  }

  minikv::Env* env = minikv::Env::Default();
  if (!EnsureDir(env, "./data") || !EnsureDir(env, dbroot)) {
    std::cerr << "cannot create db dir: " << dbroot << "\n";
    return 1;
  }

  const std::string kvdir = dbroot + "/minikv";
  const std::string walpath = dbroot + "/simple.wal";
  if (!EnsureDir(env, kvdir)) {
    std::cerr << "cannot create " << kvdir << "\n";
    return 1;
  }
  minikv::bench::CleanupEngineDir(kvdir);
  if (env->FileExists(walpath)) {
    env->DeleteFile(walpath).ok();
  }

  std::printf("MiniKV bench  ops=%d value_bytes=%d db=%s\n", ops, value_bytes,
              dbroot.c_str());

  {
    minikv::bench::MemoryMapStore mem;
    const uint64_t a = minikv::bench::NowMicros();
    for (int i = 0; i < ops; ++i) {
      mem.Put(minikv::bench::MakeKey(i),
              minikv::bench::MakeValue(i, static_cast<size_t>(value_bytes)));
    }
    const uint64_t b = minikv::bench::NowMicros();
    minikv::bench::PrintStats(
        {"baseline.memory_map.write", static_cast<uint64_t>(ops), b - a});
    std::string v;
    const uint64_t c = minikv::bench::NowMicros();
    for (int i = 0; i < ops; ++i) {
      mem.Get(minikv::bench::MakeKey(i), &v);
    }
    const uint64_t d = minikv::bench::NowMicros();
    minikv::bench::PrintStats(
        {"baseline.memory_map.get", static_cast<uint64_t>(ops), d - c});
  }

  {
    minikv::bench::SimpleWalMapStore simple(walpath);
    minikv::Status s = simple.Open(true);
    if (!s.ok()) {
      std::cerr << s.ToString() << "\n";
      return 1;
    }
    const uint64_t a = minikv::bench::NowMicros();
    for (int i = 0; i < ops; ++i) {
      simple.Put(minikv::bench::MakeKey(i),
                 minikv::bench::MakeValue(i, static_cast<size_t>(value_bytes)));
    }
    const uint64_t b = minikv::bench::NowMicros();
    minikv::bench::PrintStats(
        {"baseline.simple_wal.write", static_cast<uint64_t>(ops), b - a});
    simple.Close().ok();
  }

  {
    minikv::Options options;
    options.create_if_missing = true;
    // 略小的 buffer，便于默认 ops 规模下也能看到 Flush/SST（非压测极限调优）。
    options.write_buffer_size = 64 * 1024;
    options.compaction_trigger = 4;
    options.sync = false;

    minikv::DB* db = nullptr;
    minikv::Status s = minikv::DB::Open(options, kvdir, &db);
    if (!s.ok()) {
      std::cerr << s.ToString() << "\n";
      return 1;
    }
    minikv::WriteOptions w;
    w.sync = false;
    const uint64_t a = minikv::bench::NowMicros();
    for (int i = 0; i < ops; ++i) {
      s = db->Put(w, minikv::bench::MakeKey(i),
                  minikv::bench::MakeValue(i, static_cast<size_t>(value_bytes)));
      if (!s.ok()) {
        std::cerr << s.ToString() << "\n";
        db->Close().ok();
        delete db;
        return 1;
      }
    }
    const uint64_t b = minikv::bench::NowMicros();
    minikv::bench::PrintStats(
        {"minikv.seq_write", static_cast<uint64_t>(ops), b - a});

    auto* impl = static_cast<minikv::DBImpl*>(db);
    impl->WaitForBackgroundForTest();

    std::string v;
    minikv::ReadOptions r;
    const uint64_t c = minikv::bench::NowMicros();
    for (int i = 0; i < ops; ++i) {
      s = db->Get(r, minikv::bench::MakeKey(i), &v);
      if (!s.ok()) {
        std::cerr << "get failed " << i << " " << s.ToString() << "\n";
        db->Close().ok();
        delete db;
        return 1;
      }
    }
    const uint64_t d = minikv::bench::NowMicros();
    minikv::bench::PrintStats(
        {"minikv.point_get", static_cast<uint64_t>(ops), d - c});

    std::printf("  minikv.sst_count=%zu compaction_count=%llu\n",
                impl->SstCountForTest(),
                static_cast<unsigned long long>(impl->CompactionCountForTest()));

    db->Close().ok();
    delete db;
  }

  std::printf(
      "说明: memory_map 无持久化；simple_wal 仅追加日志；"
      "MiniKV 含 WAL+MemTable+Flush+SST+Compaction。\n");
  return 0;
}
