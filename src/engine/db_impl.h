#pragma once

#include "minikv/db.h"

#include "engine/memtable.h"
#include "engine/table.h"
#include "engine/version_edit.h"
#include "engine/wal.h"
#include "engine/write_batch.h"
#include "env.h"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace minikv {

class DBImpl : public DB {
 public:
  DBImpl(const Options& options, std::string dbname);
  ~DBImpl() override;

  Status Open();

  Status Put(const WriteOptions& options, const Slice& key,
             const Slice& value) override;
  Status Delete(const WriteOptions& options, const Slice& key) override;
  Status Get(const ReadOptions& options, const Slice& key,
             std::string* value) override;
  Status Close() override;

  void WaitForBackgroundForTest();
  void SetBackgroundPausedForTest(bool paused);
  void SetBackgroundHookForTest(std::function<void(const char*)> hook);
  size_t SstCountForTest() const;
  uint64_t CompactionCountForTest() const;

 private:
  Status Recover();
  Status Write(const WriteOptions& options, WriteBatch* updates);
  Status MakeRoomForWrite(std::unique_lock<std::mutex>& lock);
  Status FlushImmTable(std::unique_lock<std::mutex>& lock);
  Status CompactSizeTiered(std::unique_lock<std::mutex>& lock);
  bool NeedCompactionLocked() const;
  void BackgroundThreadMain();
  void MaybeScheduleBackgroundWork();
  Status RotateMemTableLog(uint64_t new_log_number);
  Status PersistManifest();
  Status PersistManifestWithTables(const std::vector<uint64_t>& table_numbers,
                                   uint64_t prev_log_number);
  Status GetFromTables(const Slice& user_key, std::string* value);

  const Options options_;
  const std::string dbname_;
  Env* env_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;

  MemTable* mem_ = nullptr;
  MemTable* imm_ = nullptr;
  std::unique_ptr<WalWriter> log_;
  SequenceNumber last_sequence_ = 0;
  uint64_t log_number_ = 1;
  uint64_t prev_log_number_ = 0;
  uint64_t next_file_number_ = 2;
  std::vector<std::unique_ptr<Table>> tables_;  // 旧 → 新

  bool closed_ = true;
  bool shutting_down_ = false;
  bool bg_work_scheduled_ = false;
  bool bg_paused_for_test_ = false;
  std::function<void(const char*)> bg_hook_for_test_;
  uint64_t compaction_count_ = 0;
  Status bg_error_;
  std::thread bg_thread_;
};

}  // namespace minikv
