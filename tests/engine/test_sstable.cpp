#include "engine/dbformat.h"
#include "engine/table.h"
#include "env.h"
#include "tests/testharness.h"

#include <string>

int main() {
  minikv::test::LogSection("TEST SUITE: test_sstable (小节 S6/S7 · SSTable 文件)");

  minikv::Env* env = minikv::Env::Default();
  const std::string dir = minikv::test::MakeTestDir("sstable");
  const std::string path = dir + "/demo.sst";
  minikv::test::RemoveFileIfExists(path);

  {
    minikv::test::LogStep("build SSTable with three keys (a,b,c)");
    minikv::TableBuilder builder(env, path);
    std::string ikey;
    minikv::AppendInternalKey(&ikey, "a", 1, minikv::kTypeValue);
    builder.Add(ikey, "va");
    ikey.clear();
    minikv::AppendInternalKey(&ikey, "b", 2, minikv::kTypeValue);
    builder.Add(ikey, "vb");
    ikey.clear();
    minikv::AppendInternalKey(&ikey, "c", 3, minikv::kTypeDeletion);
    builder.Add(ikey, "");
    CHECK_OK(builder.Finish());
    minikv::test::Log("  entries=" + std::to_string(builder.NumEntries()) +
                      " bytes=" + std::to_string(builder.FileSize()));
  }

  {
    minikv::test::LogStep("open SSTable and point-lookup");
    std::unique_ptr<minikv::Table> table;
    CHECK_OK(minikv::Table::Open(env, path, &table));

    std::string value;
    bool found = false;
    CHECK_OK(table->Get("a", 100, &value, &found));
    CHECK(found);
    CHECK_EQ(value, std::string("va"));

    found = false;
    CHECK_OK(table->Get("b", 100, &value, &found));
    CHECK(found);
    CHECK_EQ(value, std::string("vb"));

    found = false;
    minikv::Status s = table->Get("c", 100, &value, &found);
    CHECK(found);
    CHECK(s.IsNotFound());
    CHECK_EQ(s.message(), std::string("deleted"));

    found = false;
    CHECK_OK(table->Get("z", 100, &value, &found));
    CHECK(!found);
  }

  minikv::test::RemoveFileIfExists(path);
  return minikv::test::Report("test_sstable");
}
