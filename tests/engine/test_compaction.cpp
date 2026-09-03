#include "engine/compaction.h"
#include "engine/db_impl.h"
#include "engine/dbformat.h"
#include "engine/filename.h"
#include "engine/table.h"
#include "engine/version_edit.h"
#include "minikv/db.h"
#include "tests/testharness.h"

#include <string>

namespace {

void CleanupDbDir(const std::string& dbname) {
  minikv::test::RemoveFileIfExists(dbname + "/MANIFEST");
  minikv::test::RemoveFileIfExists(dbname + "/MANIFEST.tmp");
  for (int i = 1; i < 64; ++i) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s/%06d.log", dbname.c_str(), i);
    minikv::test::RemoveFileIfExists(buf);
    std::snprintf(buf, sizeof(buf), "%s/%06d.sst", dbname.c_str(), i);
    minikv::test::RemoveFileIfExists(buf);
  }
}

}  // namespace

int main() {
  minikv::test::LogSection("TEST SUITE: test_compaction (小节 S8 · Size-Tiered)");

  {
    minikv::test::LogStep("unit: SizeTierBucket is monotonic for growing sizes");
    CHECK(minikv::SizeTierBucket(100) <= minikv::SizeTierBucket(10000));
    CHECK(minikv::SizeTierBucket(10000) <= minikv::SizeTierBucket(1000000));
  }

  const std::string dbname = minikv::test::MakeTestDir("compaction_db");
  CleanupDbDir(dbname);

  minikv::Options options;
  options.create_if_missing = true;
  options.write_buffer_size = 8 * 1024;
  options.compaction_trigger = 4;
  options.disable_auto_compaction = false;
  options.sync = false;

  minikv::DB* db = nullptr;
  CHECK_OK(minikv::DB::Open(options, dbname, &db));
  auto* impl = static_cast<minikv::DBImpl*>(db);

  minikv::WriteOptions wopt;
  wopt.sync = false;

  minikv::test::LogStep("write enough data to produce many similar-sized SSTs");
  for (int i = 0; i < 1200; ++i) {
    char key[32];
    char val[256];
    std::snprintf(key, sizeof(key), "k%05d", i);
    std::snprintf(val, sizeof(val),
                  "value-%05d-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", i);
    CHECK_OK_QUIET(db->Put(wopt, key, val));
  }
  minikv::test::Log("  wrote 1200 keys");

  minikv::test::LogStep("wait flush + size-tiered compaction to settle");
  impl->WaitForBackgroundForTest();

  const size_t sst_count = impl->SstCountForTest();
  const uint64_t compact_n = impl->CompactionCountForTest();
  minikv::test::Log("  sst_count=" + std::to_string(sst_count));
  minikv::test::Log("  compaction_count=" + std::to_string(compact_n));
  CHECK(compact_n >= 1);
  // 触发阈值为 4：合并应显著压低「只 Flush、不合并」时的文件数。
  CHECK(sst_count < 40);

  minikv::test::LogStep("spot-check values still correct after compaction");
  std::string value;
  minikv::ReadOptions ropt;
  CHECK_OK(db->Get(ropt, "k00000", &value));
  CHECK_EQ(value,
           std::string("value-00000-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"));
  CHECK_OK(db->Get(ropt, "k01199", &value));
  CHECK_EQ(value,
           std::string("value-01199-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"));

  minikv::test::LogStep("reopen after compaction; MANIFEST must load merged SSTs");
  CHECK_OK(db->Close());
  delete db;
  db = nullptr;

  CHECK_OK(minikv::DB::Open(options, dbname, &db));
  CHECK_OK(db->Get(ropt, "k00600", &value));
  CHECK_EQ(value,
           std::string("value-00600-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"));
  CHECK_OK(db->Close());
  delete db;

  minikv::test::LogStep(
      "newer SST remains authoritative when older files are compacted later");
  {
    const std::string ordered = minikv::test::MakeTestDir("compaction_versions");
    CleanupDbDir(ordered);
    std::vector<std::unique_ptr<minikv::Table>> old_tables;
    for (uint64_t number = 2; number <= 5; ++number) {
      const std::string path = minikv::TableFileName(ordered, number);
      minikv::TableBuilder builder(minikv::Env::Default(), path);
      std::string ikey;
      minikv::AppendInternalKey(&ikey, "shared", number,
                                minikv::kTypeValue);
      builder.Add(ikey, "old-" + std::to_string(number));
      CHECK_OK(builder.Finish());
      std::unique_ptr<minikv::Table> table;
      CHECK_OK(minikv::Table::Open(minikv::Env::Default(), path, &table));
      table->set_number(number);
      old_tables.push_back(std::move(table));
    }
    const std::string newer_path = minikv::TableFileName(ordered, 6);
    {
      minikv::TableBuilder builder(minikv::Env::Default(), newer_path);
      std::string ikey;
      minikv::AppendInternalKey(&ikey, "shared", 10, minikv::kTypeValue);
      builder.Add(ikey, "newest");
      CHECK_OK(builder.Finish());
    }
    std::vector<minikv::Table*> inputs;
    for (const auto& table : old_tables) inputs.push_back(table.get());
    CHECK_OK(minikv::CompactTables(minikv::Env::Default(),
                                   minikv::TableFileName(ordered, 7), inputs));
    minikv::VersionEdit edit;
    edit.next_file_number = 8;
    edit.last_sequence = 10;
    edit.log_number = 1;
    edit.sst_numbers = {6, 7};
    CHECK_OK(minikv::SaveManifest(minikv::Env::Default(), ordered, edit));
    minikv::DB* ordered_db = nullptr;
    CHECK_OK(minikv::DB::Open(options, ordered, &ordered_db));
    std::string actual;
    CHECK_OK(ordered_db->Get(minikv::ReadOptions(), "shared", &actual));
    CHECK_EQ(actual, std::string("newest"));
    CHECK_OK(ordered_db->Close());
    delete ordered_db;
  }

  return minikv::test::Report("test_compaction");
}
