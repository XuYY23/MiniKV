#include "bench/bench_common.h"
#include "engine/db_impl.h"
#include "tests/testharness.h"

#include <string>

namespace {

void CleanupDb(const std::string& dbname) {
  minikv::bench::CleanupEngineDir(dbname);
}

}  // namespace

int main() {
  minikv::test::LogSection("TEST SUITE: test_engine_e2e (小节 S9 · 整机串测+Bench)");

  const std::string root = minikv::test::MakeTestDir("engine_e2e");
  const std::string dbname = root + "/kv";
  const std::string simple_wal = root + "/simple.wal";
  CleanupDb(dbname);
  minikv::test::RemoveFileIfExists(simple_wal);
  if (!minikv::Env::Default()->FileExists(dbname)) {
    CHECK_OK(minikv::Env::Default()->CreateDir(dbname));
  }

  constexpr int kN = 800;
  constexpr size_t kValSize = 64;

  // ---------- 功能串测：走完 LSM 主链路 ----------
  minikv::test::LogStep("E2E open MiniKV with small write_buffer (force flush/compaction)");
  minikv::Options options;
  options.create_if_missing = true;
  options.write_buffer_size = 8 * 1024;
  options.compaction_trigger = 4;
  options.sync = false;

  minikv::DB* db = nullptr;
  CHECK_OK(minikv::DB::Open(options, dbname, &db));
  auto* impl = static_cast<minikv::DBImpl*>(db);

  minikv::WriteOptions wopt;
  wopt.sync = false;
  minikv::ReadOptions ropt;

  minikv::test::LogStep("sequential Put N keys");
  const uint64_t t0 = minikv::bench::NowMicros();
  for (int i = 0; i < kN; ++i) {
    CHECK_OK_QUIET(db->Put(wopt, minikv::bench::MakeKey(i),
                           minikv::bench::MakeValue(i, kValSize)));
  }
  const uint64_t t1 = minikv::bench::NowMicros();
  minikv::bench::RunStats seq_write{"minikv.seq_write",
                                    static_cast<uint64_t>(kN), t1 - t0};
  minikv::bench::PrintStats(seq_write);

  minikv::test::LogStep("wait background flush/compaction");
  impl->WaitForBackgroundForTest();
  minikv::test::Log("  sst_count=" + std::to_string(impl->SstCountForTest()) +
                    " compaction_count=" +
                    std::to_string(impl->CompactionCountForTest()));
  CHECK(impl->SstCountForTest() >= 1);

  minikv::test::LogStep("point Get all keys");
  const uint64_t t2 = minikv::bench::NowMicros();
  std::string value;
  for (int i = 0; i < kN; ++i) {
    CHECK_OK_QUIET(db->Get(ropt, minikv::bench::MakeKey(i), &value));
    CHECK_EQ_QUIET(value, minikv::bench::MakeValue(i, kValSize));
  }
  const uint64_t t3 = minikv::bench::NowMicros();
  minikv::bench::PrintStats(
      {"minikv.point_get", static_cast<uint64_t>(kN), t3 - t2});

  minikv::test::LogStep("overwrite + delete subset, then verify");
  for (int i = 0; i < kN; i += 2) {
    CHECK_OK_QUIET(db->Put(wopt, minikv::bench::MakeKey(i),
                           minikv::bench::MakeValue(i + 100000, kValSize)));
  }
  for (int i = 1; i < kN; i += 4) {
    CHECK_OK_QUIET(db->Delete(wopt, minikv::bench::MakeKey(i)));
  }
  impl->WaitForBackgroundForTest();

  int expect_deleted = 0;
  int expect_overwritten = 0;
  int expect_original = 0;
  for (int i = 0; i < kN; ++i) {
    minikv::Status s = db->Get(ropt, minikv::bench::MakeKey(i), &value);
    if (i % 4 == 1) {
      ++expect_deleted;
      if (!s.IsNotFound()) {
        CHECK(s.IsNotFound());
      }
    } else if (i % 2 == 0) {
      ++expect_overwritten;
      CHECK_OK_QUIET(s);
      CHECK_EQ_QUIET(value, minikv::bench::MakeValue(i + 100000, kValSize));
    } else {
      ++expect_original;
      CHECK_OK_QUIET(s);
      CHECK_EQ_QUIET(value, minikv::bench::MakeValue(i, kValSize));
    }
  }
  minikv::test::Log("  verified keys: deleted=" + std::to_string(expect_deleted) +
                    " overwritten=" + std::to_string(expect_overwritten) +
                    " original=" + std::to_string(expect_original));

  minikv::test::LogStep("close + reopen (MANIFEST/SST/WAL full path)");
  CHECK_OK(db->Close());
  delete db;
  db = nullptr;
  CHECK_OK(minikv::DB::Open(options, dbname, &db));
  CHECK_OK(db->Get(ropt, minikv::bench::MakeKey(0), &value));
  CHECK_EQ(value, minikv::bench::MakeValue(100000, kValSize));
  CHECK(db->Get(ropt, minikv::bench::MakeKey(1), &value).IsNotFound());
  CHECK_OK(db->Close());
  delete db;
  db = nullptr;

  // ---------- 基线对比（同机器、同规模，便于讲解） ----------
  minikv::test::LogSection("Baseline comparison (same N, relative paths)");

  {
    minikv::bench::MemoryMapStore mem;
    const uint64_t a = minikv::bench::NowMicros();
    for (int i = 0; i < kN; ++i) {
      CHECK_OK_QUIET(mem.Put(minikv::bench::MakeKey(i),
                             minikv::bench::MakeValue(i, kValSize)));
    }
    const uint64_t b = minikv::bench::NowMicros();
    minikv::bench::PrintStats(
        {"baseline.memory_map.write", static_cast<uint64_t>(kN), b - a});

    const uint64_t c = minikv::bench::NowMicros();
    for (int i = 0; i < kN; ++i) {
      CHECK_OK_QUIET(mem.Get(minikv::bench::MakeKey(i), &value));
    }
    const uint64_t d = minikv::bench::NowMicros();
    minikv::bench::PrintStats(
        {"baseline.memory_map.get", static_cast<uint64_t>(kN), d - c});
  }

  {
    minikv::bench::SimpleWalMapStore simple(simple_wal);
    CHECK_OK(simple.Open(true));
    const uint64_t a = minikv::bench::NowMicros();
    for (int i = 0; i < kN; ++i) {
      CHECK_OK_QUIET(simple.Put(minikv::bench::MakeKey(i),
                                minikv::bench::MakeValue(i, kValSize)));
    }
    const uint64_t b = minikv::bench::NowMicros();
    minikv::bench::PrintStats(
        {"baseline.simple_wal.write", static_cast<uint64_t>(kN), b - a});
    CHECK_OK(simple.Close());

    minikv::bench::SimpleWalMapStore reopen(simple_wal);
    CHECK_OK(reopen.Open(false));
    CHECK_OK(reopen.Get(minikv::bench::MakeKey(kN / 2), &value));
    CHECK_EQ(value, minikv::bench::MakeValue(kN / 2, kValSize));
    CHECK_OK(reopen.Close());
  }

  minikv::test::LogStep(
      "MiniKV includes durability and LSM maintenance; memory_map does not");
  return minikv::test::Report("test_engine_e2e");
}
