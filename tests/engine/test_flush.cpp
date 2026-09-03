#include "engine/db_impl.h"
#include "minikv/db.h"
#include "tests/testharness.h"

#include <string>

int main() {
  minikv::test::LogSection("TEST SUITE: test_flush (小节 S6 · Immutable+后台Flush)");

  const std::string dbname = minikv::test::MakeTestDir("flush_db");
  // 清理可能残留的引擎文件，保证可反复运行。
  minikv::Env* env = minikv::Env::Default();
  minikv::test::RemoveFileIfExists(dbname + "/MANIFEST");
  minikv::test::RemoveFileIfExists(dbname + "/MANIFEST.tmp");
  for (int i = 1; i < 32; ++i) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s/%06d.log", dbname.c_str(), i);
    minikv::test::RemoveFileIfExists(buf);
    std::snprintf(buf, sizeof(buf), "%s/%06d.sst", dbname.c_str(), i);
    minikv::test::RemoveFileIfExists(buf);
  }

  minikv::Options options;
  options.create_if_missing = true;
  options.write_buffer_size = 8 * 1024;  // Arena 按 4KB 块计，需多块后才会超阈值
  options.sync = false;

  minikv::DB* db = nullptr;
  CHECK_OK(minikv::DB::Open(options, dbname, &db));
  auto* impl = static_cast<minikv::DBImpl*>(db);

  minikv::WriteOptions wopt;
  wopt.sync = false;

  minikv::test::LogStep("write many kv pairs to exceed write_buffer_size");
  for (int i = 0; i < 800; ++i) {
    char key[32];
    char val[256];
    std::snprintf(key, sizeof(key), "k%04d", i);
    std::snprintf(val, sizeof(val),
                  "value-%04d-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", i);
    CHECK_OK_QUIET(db->Put(wopt, key, val));
  }
  minikv::test::Log("  wrote 800 keys (quiet checks; failures would still print)");

  minikv::test::LogStep("wait background flush to finish");
  impl->WaitForBackgroundForTest();
  const size_t sst_count = impl->SstCountForTest();
  minikv::test::Log("  sst_count=" + std::to_string(sst_count));
  CHECK(sst_count >= 1);

  minikv::test::LogStep("read key that should live in SSTable path");
  std::string value;
  minikv::ReadOptions ropt;
  CHECK_OK(db->Get(ropt, "k0000", &value));
  CHECK_EQ(value,
           std::string("value-0000-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"));
  CHECK_OK(db->Get(ropt, "k0799", &value));
  CHECK_EQ(value,
           std::string("value-0799-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"));

  minikv::test::LogStep("close and reopen: data must survive via MANIFEST+SST");
  CHECK_OK(db->Close());
  delete db;
  db = nullptr;

  CHECK_OK(minikv::DB::Open(options, dbname, &db));
  CHECK_OK(db->Get(ropt, "k0000", &value));
  CHECK_EQ(value,
           std::string("value-0000-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"));
  CHECK_OK(db->Get(ropt, "k0400", &value));
  CHECK_EQ(value,
           std::string("value-0400-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"));
  CHECK_OK(db->Close());
  delete db;

  return minikv::test::Report("test_flush");
}
