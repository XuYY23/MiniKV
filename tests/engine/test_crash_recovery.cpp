#include "minikv/db.h"
#include "engine/db_impl.h"
#include "tests/testharness.h"

#include <sys/wait.h>
#include <unistd.h>

#include <string>

namespace {
void Cleanup(const std::string& dir) {
  minikv::test::RemoveFileIfExists(dir + "/MANIFEST");
  minikv::test::RemoveFileIfExists(dir + "/MANIFEST.tmp");
  for (int i = 1; i < 256; ++i) {
    char path[256];
    std::snprintf(path, sizeof(path), "%s/%06d.log", dir.c_str(), i);
    minikv::test::RemoveFileIfExists(path);
    std::snprintf(path, sizeof(path), "%s/%06d.sst", dir.c_str(), i);
    minikv::test::RemoveFileIfExists(path);
  }
}

void RunBackgroundCrashPoint(const std::string& root,
                             const std::string& crash_point,
                             bool enable_compaction) {
  Cleanup(root);
  int progress[2];
  CHECK_EQ(::pipe(progress), 0);
  const pid_t child = ::fork();
  CHECK(child >= 0);
  if (child == 0) {
    ::close(progress[0]);
    minikv::Options options;
    options.write_buffer_size = 8 * 1024;
    options.compaction_trigger = enable_compaction ? 2 : 1000;
    minikv::DB* db = nullptr;
    if (!minikv::DB::Open(options, root, &db).ok()) ::_exit(10);
    static_cast<minikv::DBImpl*>(db)->SetBackgroundHookForTest(
        [crash_point](const char* reached) {
          if (crash_point == reached) ::_exit(91);
        });
    minikv::WriteOptions write_options;
    write_options.sync = true;
    for (int i = 0; i < 2000; ++i) {
      const std::string key = "point-key-" + std::to_string(i);
      if (!db->Put(write_options, key, std::string(256, 'v')).ok()) ::_exit(11);
      const int completed = i + 1;
      if (::write(progress[1], &completed, sizeof(completed)) !=
          static_cast<ssize_t>(sizeof(completed))) ::_exit(12);
    }
    ::_exit(13);
  }
  ::close(progress[1]);
  int completed = 0;
  int last_completed = 0;
  while (::read(progress[0], &completed, sizeof(completed)) ==
         static_cast<ssize_t>(sizeof(completed))) {
    last_completed = completed;
  }
  ::close(progress[0]);
  int status = 0;
  CHECK_EQ(::waitpid(child, &status, 0), child);
  CHECK(WIFEXITED(status));
  CHECK_EQ(WEXITSTATUS(status), 91);
  CHECK(last_completed > 0);

  minikv::Options options;
  options.write_buffer_size = 8 * 1024;
  options.compaction_trigger = enable_compaction ? 2 : 1000;
  minikv::DB* db = nullptr;
  CHECK_OK(minikv::DB::Open(options, root, &db));
  for (int i = 0; i < last_completed; ++i) {
    std::string value;
    CHECK_OK_QUIET(db->Get(minikv::ReadOptions(),
                           "point-key-" + std::to_string(i), &value));
    CHECK_EQ_QUIET(value, std::string(256, 'v'));
  }
  CHECK_OK(db->Close());
  delete db;
}
}  // namespace

int main() {
  minikv::test::LogSection("TEST SUITE: test_crash_recovery");
  const std::string dir = minikv::test::MakeTestDir("crash_recovery");
  Cleanup(dir);
  minikv::Options options;
  options.write_buffer_size = 12 * 1024;
  minikv::WriteOptions write_options;
  write_options.sync = true;

  const pid_t child = ::fork();
  CHECK(child >= 0);
  if (child == 0) {
    minikv::DB* db = nullptr;
    if (!minikv::DB::Open(options, dir, &db).ok()) ::_exit(2);
    static_cast<minikv::DBImpl*>(db)->SetBackgroundPausedForTest(true);
    for (int i = 0; i < 40; ++i) {
      const std::string key = "crash-key-" + std::to_string(i);
      const std::string value(256, static_cast<char>('a' + i % 26));
      if (!db->Put(write_options, key, value).ok()) ::_exit(3);
    }
    // Deliberately bypass destructors and DB::Close.
    ::_exit(0);
  }
  int status = 0;
  CHECK_EQ(::waitpid(child, &status, 0), child);
  CHECK(WIFEXITED(status));
  CHECK_EQ(WEXITSTATUS(status), 0);

  minikv::DB* db = nullptr;
  CHECK_OK(minikv::DB::Open(options, dir, &db));
  for (int i = 0; i < 40; ++i) {
    std::string actual;
    const std::string key = "crash-key-" + std::to_string(i);
    CHECK_OK_QUIET(db->Get(minikv::ReadOptions(), key, &actual));
    CHECK_EQ_QUIET(actual,
                   std::string(256, static_cast<char>('a' + i % 26)));
  }
  CHECK_OK(db->Close());
  delete db;

  minikv::test::LogStep("crash after SST sync but before MANIFEST install");
  RunBackgroundCrashPoint(
      minikv::test::MakeTestDir("crash_flush_before_manifest"),
      "flush_sst_synced", false);
  minikv::test::LogStep("crash after Flush MANIFEST install");
  RunBackgroundCrashPoint(
      minikv::test::MakeTestDir("crash_flush_after_manifest"),
      "flush_manifest_installed", false);
  minikv::test::LogStep("crash after Compaction MANIFEST install");
  RunBackgroundCrashPoint(
      minikv::test::MakeTestDir("crash_compaction_manifest"),
      "compaction_manifest_installed", true);
  return minikv::test::Report("test_crash_recovery");
}
