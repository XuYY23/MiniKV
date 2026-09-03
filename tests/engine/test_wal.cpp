#include "engine/wal.h"
#include "env.h"
#include "tests/testharness.h"

#include <string>

int main() {
  minikv::test::LogSection("TEST SUITE: test_wal (小节 S4)");

  minikv::Env* env = minikv::Env::Default();
  const std::string dir = minikv::test::MakeTestDir("wal");
  const std::string path = dir + "/records.log";
  minikv::test::RemoveFileIfExists(path);

  {
    minikv::test::LogStep("write two records: hello, world");
    std::unique_ptr<minikv::WritableFile> file;
    CHECK_OK(env->NewWritableFile(path, &file));
    minikv::WalWriter writer(std::move(file));
    CHECK_OK(writer.AddRecord("hello"));
    CHECK_OK(writer.AddRecord("world"));
    CHECK_OK(writer.Sync());
    CHECK_OK(writer.Close());
    minikv::test::Log("  wal file written: " + path);
  }

  {
    minikv::test::LogStep("reopen and read records back in order");
    std::unique_ptr<minikv::SequentialFile> file;
    CHECK_OK(env->NewSequentialFile(path, &file));
    minikv::WalReader reader(std::move(file));
    minikv::Slice rec;
    std::string scratch;
    minikv::Status st;

    CHECK(reader.ReadRecord(&rec, &scratch, &st));
    CHECK_OK(st);
    CHECK_EQ(rec.ToString(), std::string("hello"));

    CHECK(reader.ReadRecord(&rec, &scratch, &st));
    CHECK_OK(st);
    CHECK_EQ(rec.ToString(), std::string("world"));

    minikv::test::LogStep("third ReadRecord should hit EOF cleanly");
    CHECK(!reader.ReadRecord(&rec, &scratch, &st));
    CHECK_OK(st);
  }

  minikv::test::RemoveFileIfExists(path);
  return minikv::test::Report("test_wal");
}
