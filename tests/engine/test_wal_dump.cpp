#include "engine/wal.h"
#include "engine/write_batch.h"
#include "env.h"
#include "tests/testharness.h"

#include <string>
#include <vector>

namespace {

class CollectHandler : public minikv::WriteBatch::Handler {
 public:
  void Put(const minikv::Slice& key, const minikv::Slice& value) override {
    lines.push_back("Put " + key.ToString() + "=" + value.ToString());
  }
  void Delete(const minikv::Slice& key) override {
    lines.push_back("Delete " + key.ToString());
  }
  std::vector<std::string> lines;
};

}  // namespace

int main() {
  minikv::test::LogSection("TEST SUITE: test_wal_dump (WAL 可读解码)");

  minikv::Env* env = minikv::Env::Default();
  const std::string dir = minikv::test::MakeTestDir("wal_dump");
  const std::string path = dir + "/demo.log";
  minikv::test::RemoveFileIfExists(path);

  minikv::test::LogStep("write WriteBatch records into WAL (Put/Delete)");
  {
    std::unique_ptr<minikv::WritableFile> file;
    CHECK_OK(env->NewWritableFile(path, &file));
    minikv::WalWriter writer(std::move(file));

    minikv::WriteBatch b1;
    b1.SetSequence(1);
    b1.Put("k1", "v1");
    CHECK_OK(writer.AddRecord(b1.Contents()));

    minikv::WriteBatch b2;
    b2.SetSequence(2);
    b2.Put("k2", "v2");
    CHECK_OK(writer.AddRecord(b2.Contents()));

    minikv::WriteBatch b3;
    b3.SetSequence(3);
    b3.Delete("k1");
    CHECK_OK(writer.AddRecord(b3.Contents()));

    CHECK_OK(writer.Sync());
    CHECK_OK(writer.Close());
    minikv::test::Log("  wal written: " + path);
  }

  minikv::test::LogStep("read WAL and decode each WriteBatch (same path as dump tool)");
  {
    std::unique_ptr<minikv::SequentialFile> file;
    CHECK_OK(env->NewSequentialFile(path, &file));
    minikv::WalReader reader(std::move(file));

    minikv::Slice payload;
    std::string scratch;
    minikv::Status st;
    CollectHandler all;
    int records = 0;

    while (true) {
      const bool ok = reader.ReadRecord(&payload, &scratch, &st);
      CHECK_OK(st);
      if (!ok) {
        break;
      }
      ++records;
      minikv::WriteBatch batch;
      batch.SetContents(payload);
      minikv::test::Log("  record#" + std::to_string(records) +
                        " seq=" + std::to_string(batch.Sequence()) +
                        " count=" + std::to_string(batch.Count()));

      CollectHandler one;
      CHECK_OK(batch.Iterate(&one));
      for (const auto& line : one.lines) {
        minikv::test::Log("    " + line);
        all.lines.push_back(line);
      }
    }

    CHECK_EQ(records, 3);
    CHECK_EQ(static_cast<int>(all.lines.size()), 3);
    CHECK_EQ(all.lines[0], std::string("Put k1=v1"));
    CHECK_EQ(all.lines[1], std::string("Put k2=v2"));
    CHECK_EQ(all.lines[2], std::string("Delete k1"));
  }

  minikv::test::LogStep(
      "tip: demo with tool -> ./build/minikv_wal_dump " + path);
  return minikv::test::Report("test_wal_dump");
}
