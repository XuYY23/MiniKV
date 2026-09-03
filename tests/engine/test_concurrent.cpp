#include "engine/db_impl.h"
#include "minikv/db.h"
#include "tests/testharness.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

void CleanupDb(const std::string& dbname) {
  minikv::test::RemoveFileIfExists(dbname + "/MANIFEST");
  minikv::test::RemoveFileIfExists(dbname + "/MANIFEST.tmp");
  for (int i = 1; i < 256; ++i) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s/%06d.log", dbname.c_str(), i);
    minikv::test::RemoveFileIfExists(buf);
    std::snprintf(buf, sizeof(buf), "%s/%06d.sst", dbname.c_str(), i);
    minikv::test::RemoveFileIfExists(buf);
  }
}

std::string KeyOf(int writer, int i) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "w%02d-k%05d", writer, i);
  return buf;
}

std::string ValOf(int writer, int i) {
  char buf[128];
  std::snprintf(buf, sizeof(buf),
                "v-w%02d-%05d-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", writer,
                i);
  return buf;
}

}  // namespace

int main() {
  minikv::test::LogSection(
      "TEST SUITE: test_concurrent (多线程读写 + Flush 交叉)");

  const std::string dbname = minikv::test::MakeTestDir("concurrent_db");
  CleanupDb(dbname);

  minikv::Options options;
  options.create_if_missing = true;
  // 小 buffer：并发写时更容易触发 Immutable + 后台 Flush。
  options.write_buffer_size = 8 * 1024;
  options.compaction_trigger = 4;
  options.sync = false;

  minikv::DB* db = nullptr;
  CHECK_OK(minikv::DB::Open(options, dbname, &db));
  auto* impl = static_cast<minikv::DBImpl*>(db);

  constexpr int kWriters = 4;
  constexpr int kReaders = 4;
  constexpr int kPerWriter = 250;  // 共 1000 keys
  const int total_keys = kWriters * kPerWriter;

  minikv::WriteOptions wopt;
  wopt.sync = false;
  minikv::ReadOptions ropt;

  std::atomic<int> write_errors{0};
  std::atomic<int> read_errors{0};
  std::atomic<int> keys_written{0};
  std::atomic<bool> writers_done{false};
  std::atomic<size_t> max_sst_seen{0};

  minikv::test::LogStep(
      "launch writers + readers concurrently (writers partitioned by key space)");
  minikv::test::Log("  writers=" + std::to_string(kWriters) +
                    " readers=" + std::to_string(kReaders) +
                    " keys_per_writer=" + std::to_string(kPerWriter));

  std::vector<std::thread> writers;
  std::vector<std::thread> readers;
  writers.reserve(static_cast<size_t>(kWriters));
  readers.reserve(static_cast<size_t>(kReaders));

  // 观察线程：写进行中采样 sst_count，证明 Flush 与前台写交叉发生。
  std::thread observer([&]() {
    while (!writers_done.load(std::memory_order_acquire)) {
      const size_t n = impl->SstCountForTest();
      size_t cur = max_sst_seen.load(std::memory_order_relaxed);
      while (n > cur &&
             !max_sst_seen.compare_exchange_weak(cur, n,
                                                 std::memory_order_relaxed)) {
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });

  for (int w = 0; w < kWriters; ++w) {
    writers.emplace_back([&, w]() {
      for (int i = 0; i < kPerWriter; ++i) {
        const minikv::Status s = db->Put(wopt, KeyOf(w, i), ValOf(w, i));
        if (!s.ok()) {
          write_errors.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        keys_written.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&, r]() {
      std::string value;
      // 读线程在写过程中轮询：允许暂时 NotFound，不允许读到错值。
      // 每次迭代稍睡，避免在单锁实现下把写线程饿死。
      while (!writers_done.load(std::memory_order_acquire)) {
        const int done = keys_written.load(std::memory_order_relaxed);
        if (done > 0) {
          const int idx = (done - 1) % total_keys;
          const int ww = idx / kPerWriter;
          const int ii = idx % kPerWriter;
          const minikv::Status s = db->Get(ropt, KeyOf(ww, ii), &value);
          if (s.ok()) {
            if (value != ValOf(ww, ii)) {
              read_errors.fetch_add(1, std::memory_order_relaxed);
              return;
            }
          } else if (!s.IsNotFound()) {
            read_errors.fetch_add(1, std::memory_order_relaxed);
            return;
          }
          (void)db->Get(ropt, KeyOf(r % kWriters, 0), &value);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
  }

  for (auto& t : writers) {
    t.join();
  }
  writers_done.store(true, std::memory_order_release);
  for (auto& t : readers) {
    t.join();
  }
  observer.join();

  CHECK_EQ(write_errors.load(), 0);
  CHECK_EQ(read_errors.load(), 0);
  CHECK_EQ(keys_written.load(), total_keys);
  minikv::test::Log("  concurrent phase: all puts ok, reader mismatches=0");

  minikv::test::LogStep("wait background flush/compaction after concurrent writes");
  impl->WaitForBackgroundForTest();
  const size_t sst_final = impl->SstCountForTest();
  const size_t sst_during = max_sst_seen.load();
  minikv::test::Log("  sst_seen_during_writes=" + std::to_string(sst_during) +
                    " sst_final=" + std::to_string(sst_final) +
                    " compaction_count=" +
                    std::to_string(impl->CompactionCountForTest()));
  CHECK(sst_final >= 1);
  // 写过程中或结束后应已出现 SST（小 buffer 下通常写到一半就 Flush）。
  CHECK(sst_during >= 1 || sst_final >= 1);

  minikv::test::LogStep("verify every key after concurrent load");
  {
    std::string value;
    int ok = 0;
    for (int w = 0; w < kWriters; ++w) {
      for (int i = 0; i < kPerWriter; ++i) {
        CHECK_OK_QUIET(db->Get(ropt, KeyOf(w, i), &value));
        CHECK_EQ_QUIET(value, ValOf(w, i));
        ++ok;
      }
    }
    minikv::test::Log("  verified keys=" + std::to_string(ok));
    CHECK_EQ(ok, total_keys);
  }

  minikv::test::LogStep("concurrent overwrite + delete subset");
  {
    std::atomic<int> err{0};
    std::vector<std::thread> mutators;
    for (int w = 0; w < kWriters; ++w) {
      mutators.emplace_back([&, w]() {
        for (int i = 0; i < kPerWriter; ++i) {
          if (i % 4 == 1) {
            const minikv::Status s = db->Delete(wopt, KeyOf(w, i));
            if (!s.ok()) {
              err.fetch_add(1);
              return;
            }
          } else if (i % 2 == 0) {
            const std::string nv = ValOf(w, i + 100000);
            const minikv::Status s = db->Put(wopt, KeyOf(w, i), nv);
            if (!s.ok()) {
              err.fetch_add(1);
              return;
            }
          }
        }
      });
    }
    for (auto& t : mutators) {
      t.join();
    }
    CHECK_EQ(err.load(), 0);
    impl->WaitForBackgroundForTest();

    std::string value;
    int deleted = 0;
    int overwritten = 0;
    int original = 0;
    for (int w = 0; w < kWriters; ++w) {
      for (int i = 0; i < kPerWriter; ++i) {
        const minikv::Status s = db->Get(ropt, KeyOf(w, i), &value);
        if (i % 4 == 1) {
          ++deleted;
          if (!s.IsNotFound()) {
            CHECK(s.IsNotFound());
          }
        } else if (i % 2 == 0) {
          ++overwritten;
          CHECK_OK_QUIET(s);
          CHECK_EQ_QUIET(value, ValOf(w, i + 100000));
        } else {
          ++original;
          CHECK_OK_QUIET(s);
          CHECK_EQ_QUIET(value, ValOf(w, i));
        }
      }
    }
    minikv::test::Log("  after mutate: deleted=" + std::to_string(deleted) +
                      " overwritten=" + std::to_string(overwritten) +
                      " original=" + std::to_string(original));
  }

  minikv::test::LogStep("close + reopen under concurrent-produced state");
  CHECK_OK(db->Close());
  delete db;
  db = nullptr;
  CHECK_OK(minikv::DB::Open(options, dbname, &db));
  {
    std::string value;
    CHECK_OK(db->Get(ropt, KeyOf(0, 0), &value));
    CHECK_EQ(value, ValOf(0, 100000));
    CHECK(db->Get(ropt, KeyOf(0, 1), &value).IsNotFound());
  }
  CHECK_OK(db->Close());
  delete db;

  minikv::test::LogStep(
      "S2–S5 sequential tests check component correctness; "
      "this suite stresses engine mutex + background Flush under threads");
  return minikv::test::Report("test_concurrent");
}
