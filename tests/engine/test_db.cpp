#include "minikv/db.h"
#include "tests/testharness.h"

#include <string>

int main() {
  minikv::test::LogSection("TEST SUITE: test_db (小节 S5)");

  const std::string dbname = minikv::test::MakeTestDir("db_recover");
  minikv::test::RemoveFileIfExists(dbname + "/MANIFEST");
  minikv::test::RemoveFileIfExists(dbname + "/MANIFEST.tmp");
  for (int i = 1; i < 64; ++i) {
    char path[128];
    std::snprintf(path, sizeof(path), "%s/%06d.log", dbname.c_str(), i);
    minikv::test::RemoveFileIfExists(path);
    std::snprintf(path, sizeof(path), "%s/%06d.sst", dbname.c_str(), i);
    minikv::test::RemoveFileIfExists(path);
  }

  minikv::Options options;
  options.create_if_missing = true;
  options.error_if_exists = false;

  minikv::WriteOptions wopt;
  wopt.sync = true;
  minikv::ReadOptions ropt;
  std::string value;

  {
    minikv::test::LogStep("Open fresh DB and Put k1/k2, Delete k1");
    minikv::DB* db = nullptr;
    CHECK_OK(minikv::DB::Open(options, dbname, &db));
    CHECK_OK(db->Put(wopt, "k1", "v1"));
    CHECK_OK(db->Put(wopt, "k2", "v2"));
    CHECK_OK(db->Delete(wopt, "k1"));

    minikv::test::LogStep("in-memory view before close");
    CHECK(db->Get(ropt, "k1", &value).IsNotFound());
    CHECK_OK(db->Get(ropt, "k2", &value));
    CHECK_EQ(value, std::string("v2"));
    CHECK_OK(db->Close());
    delete db;
  }

  {
    minikv::test::LogStep("re-Open: WAL recovery must keep Delete(k1) and Put(k2)");
    minikv::DB* db = nullptr;
    CHECK_OK(minikv::DB::Open(options, dbname, &db));
    CHECK(db->Get(ropt, "k1", &value).IsNotFound());
    CHECK_OK(db->Get(ropt, "k2", &value));
    CHECK_EQ(value, std::string("v2"));

    minikv::test::LogStep("Put k3 then close again");
    CHECK_OK(db->Put(wopt, "k3", "v3"));
    CHECK_OK(db->Close());
    delete db;
  }

  {
    minikv::test::LogStep("second recovery: k3 must still be readable");
    minikv::DB* db = nullptr;
    CHECK_OK(minikv::DB::Open(options, dbname, &db));
    CHECK_OK(db->Get(ropt, "k3", &value));
    CHECK_EQ(value, std::string("v3"));
    CHECK_OK(db->Close());
    delete db;
  }

  return minikv::test::Report("test_db");
}
