#include "engine/db_impl.h"

#include "engine/compaction.h"
#include "engine/dbformat.h"
#include "engine/filename.h"
#include "engine/table.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <set>

namespace minikv {

DBImpl::DBImpl(const Options& options, std::string dbname)
    : options_(options),
      dbname_(std::move(dbname)),
      env_(Env::Default()) {}

DBImpl::~DBImpl() {
  if (!closed_) {
    Close().ok();
  }
}

Status DBImpl::PersistManifest() {
  std::vector<uint64_t> numbers;
  numbers.reserve(tables_.size());
  for (const auto& t : tables_) {
    numbers.push_back(t->number());
  }
  return PersistManifestWithTables(numbers, prev_log_number_);
}

Status DBImpl::PersistManifestWithTables(
    const std::vector<uint64_t>& table_numbers, uint64_t prev_log_number) {
  VersionEdit edit;
  edit.next_file_number = next_file_number_;
  edit.last_sequence = last_sequence_;
  edit.log_number = log_number_;
  edit.prev_log_number = prev_log_number;
  edit.sst_numbers = table_numbers;
  return SaveManifest(env_, dbname_, edit);
}

Status DBImpl::RotateMemTableLog(uint64_t new_log_number) {
  const uint64_t old_log_number = log_number_;
  if (log_) {
    Status s = log_->Sync();
    if (!s.ok()) {
      return s;
    }
    s = log_->Close();
    if (!s.ok()) {
      return s;
    }
    log_.reset();
  }

  log_number_ = new_log_number;
  std::unique_ptr<WritableFile> file;
  Status s = env_->NewWritableFile(LogFileName(dbname_, log_number_), &file);
  if (!s.ok()) {
    return s;
  }
  log_.reset(new WalWriter(std::move(file)));
  prev_log_number_ = old_log_number;
  return PersistManifest();
}

Status DBImpl::Open() {
  if (options_.write_buffer_size == 0) {
    return Status::InvalidArgument("write_buffer_size must be greater than zero");
  }
  if (!options_.disable_auto_compaction && options_.compaction_trigger < 2) {
    return Status::InvalidArgument("compaction_trigger must be at least two");
  }
  if (env_->FileExists(dbname_) && env_->FileIsDirectory(dbname_)) {
    if (options_.error_if_exists) {
      return Status::InvalidArgument("database already exists: " + dbname_);
    }
  } else {
    if (!options_.create_if_missing) {
      return Status::InvalidArgument("database missing: " + dbname_);
    }
    Status s = env_->CreateDir(dbname_);
    if (!s.ok()) {
      return s;
    }
  }

  mem_ = new MemTable();
  mem_->Ref();

  Status s = Recover();
  if (!s.ok()) {
    mem_->Unref();
    mem_ = nullptr;
    return s;
  }

  std::unique_ptr<WritableFile> file;
  s = env_->NewAppendableFile(LogFileName(dbname_, log_number_), &file);
  if (!s.ok()) {
    mem_->Unref();
    mem_ = nullptr;
    return s;
  }
  log_.reset(new WalWriter(std::move(file)));

  // A previous WAL in the manifest means the process stopped during a Flush.
  // Consolidate both recovered logs before accepting new writes so a later
  // MemTable rotation never overwrites the outstanding previous-log slot.
  if (prev_log_number_ != 0 && mem_->ApproximateMemoryUsage() > 0) {
    imm_ = mem_;
    mem_ = new MemTable();
    mem_->Ref();
    std::unique_lock<std::mutex> lock(mutex_);
    s = FlushImmTable(lock);
    if (!s.ok()) {
      mem_->Unref();
      mem_ = nullptr;
      imm_->Unref();
      imm_ = nullptr;
      return s;
    }
  }

  closed_ = false;
  shutting_down_ = false;
  bg_thread_ = std::thread(&DBImpl::BackgroundThreadMain, this);
  return Status::OK();
}

Status DBImpl::Recover() {
  VersionEdit edit;
  bool found = false;
  Status s = LoadManifest(env_, dbname_, &edit, &found);
  if (!s.ok()) {
    return s;
  }

  if (found) {
    next_file_number_ = std::max<uint64_t>(edit.next_file_number, 2);
    last_sequence_ = edit.last_sequence;
    log_number_ = std::max<uint64_t>(edit.log_number, 1);
    prev_log_number_ = edit.prev_log_number;
    for (uint64_t n : edit.sst_numbers) {
      std::unique_ptr<Table> table;
      s = Table::Open(env_, TableFileName(dbname_, n), &table);
      if (!s.ok()) {
        return s;
      }
      table->set_number(n);
      tables_.push_back(std::move(table));
    }
  } else {
    next_file_number_ = 2;
    log_number_ = 1;
    last_sequence_ = 0;
    s = PersistManifest();
    if (!s.ok()) {
      return s;
    }
  }

  SequenceNumber max_seq = last_sequence_;

  std::vector<uint64_t> logs;
  if (prev_log_number_ != 0) {
    logs.push_back(prev_log_number_);
  }
  logs.push_back(log_number_);
  for (uint64_t number : logs) {
    const std::string log_path = LogFileName(dbname_, number);
    if (!env_->FileExists(log_path)) {
      if (number == prev_log_number_) {
        return Status::Corruption("referenced previous WAL is missing");
      }
      std::unique_ptr<WritableFile> bootstrap_file;
      s = env_->NewWritableFile(log_path, &bootstrap_file);
      if (!s.ok()) return s;
      WalWriter bootstrap(std::move(bootstrap_file));
      s = bootstrap.Close();
      if (!s.ok()) return s;
      continue;
    }

    std::unique_ptr<SequentialFile> file;
    s = env_->NewSequentialFile(log_path, &file);
    if (!s.ok()) return s;
    WalReader reader(std::move(file));
    Slice record;
    std::string scratch;
    while (true) {
      Status read_status;
      if (!reader.ReadRecord(&record, &scratch, &read_status)) {
        if (!read_status.ok()) return read_status;
        break;
      }
      WriteBatch batch;
      batch.SetContents(record);
      s = batch.InsertInto(mem_);
      if (!s.ok()) return s;
      if (batch.Count() > 0) {
        const SequenceNumber end_seq =
            batch.Sequence() + static_cast<SequenceNumber>(batch.Count()) - 1;
        max_seq = std::max(max_seq, end_seq);
      }
    }
  }

  last_sequence_ = max_seq;
  return Status::OK();
}

void DBImpl::MaybeScheduleBackgroundWork() {
  if (shutting_down_ || !bg_error_.ok()) {
    return;
  }
  const bool need =
      (imm_ != nullptr) || NeedCompactionLocked();
  if (need) {
    bg_work_scheduled_ = true;
    cv_.notify_all();
  }
}

bool DBImpl::NeedCompactionLocked() const {
  if (options_.disable_auto_compaction) {
    return false;
  }
  std::vector<size_t> inputs;
  return PickSizeTieredCompaction(tables_, options_.compaction_trigger, &inputs);
}

void DBImpl::BackgroundThreadMain() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (true) {
    while (!shutting_down_ && !bg_work_scheduled_) {
      cv_.wait(lock);
    }
    while (!shutting_down_ && bg_paused_for_test_) {
      cv_.wait(lock);
    }
    if (shutting_down_ && !bg_work_scheduled_ &&
        (imm_ == nullptr || !bg_error_.ok())) {
      break;
    }
    bg_work_scheduled_ = false;

    Status s = Status::OK();
    if (imm_ != nullptr) {
      s = FlushImmTable(lock);
    }
    if (s.ok()) {
      while (NeedCompactionLocked()) {
        s = CompactSizeTiered(lock);
        if (!s.ok()) {
          break;
        }
      }
    }
    if (!s.ok() && bg_error_.ok()) {
      bg_error_ = s;
    }
    // 若仍有工作（例如 flush 后又满足 compaction），继续调度。
    MaybeScheduleBackgroundWork();
    cv_.notify_all();
  }
}

Status DBImpl::FlushImmTable(std::unique_lock<std::mutex>& lock) {
  if (imm_ == nullptr) {
    return Status::OK();
  }

  MemTable* imm = imm_;
  const uint64_t sst_number = next_file_number_++;
  const std::string fname = TableFileName(dbname_, sst_number);

  lock.unlock();

  Status s = Status::OK();
  bool has_entries = false;
  {
    TableBuilder builder(env_, fname);
    std::unique_ptr<MemTable::Iterator> it(imm->NewIterator());
    it->SeekToFirst();
    while (it->Valid()) {
      has_entries = true;
      builder.Add(it->key(), it->value());
      it->Next();
    }
    if (has_entries) {
      s = builder.Finish();
    } else {
      s = builder.Abandon();
    }
  }

  std::unique_ptr<Table> table;
  if (s.ok() && has_entries) {
    s = Table::Open(env_, fname, &table);
    if (s.ok()) {
      table->set_number(sst_number);
    }
  }
  if (s.ok() && bg_hook_for_test_) bg_hook_for_test_("flush_sst_synced");

  lock.lock();
  if (!s.ok()) {
    if (has_entries) {
      env_->DeleteFile(fname).ok();
    }
    return s;
  }

  if (has_entries) {
    std::vector<uint64_t> new_tables;
    new_tables.reserve(tables_.size() + 1);
    for (const auto& existing : tables_) {
      new_tables.push_back(existing->number());
    }
    new_tables.push_back(sst_number);
    s = PersistManifestWithTables(new_tables, /*prev_log_number=*/0);
    if (!s.ok()) {
      env_->DeleteFile(fname).ok();
      return s;
    }
    if (bg_hook_for_test_) bg_hook_for_test_("flush_manifest_installed");
    tables_.push_back(std::move(table));
    const uint64_t obsolete_log = prev_log_number_;
    prev_log_number_ = 0;
    if (obsolete_log != 0) {
      const std::string old_path = LogFileName(dbname_, obsolete_log);
      if (env_->FileExists(old_path)) {
        env_->DeleteFile(old_path).ok();
        env_->SyncDir(dbname_).ok();
      }
    }
  }

  imm_->Unref();
  imm_ = nullptr;
  return Status::OK();
}

Status DBImpl::CompactSizeTiered(std::unique_lock<std::mutex>& lock) {
  std::vector<size_t> input_idxs;
  if (!PickSizeTieredCompaction(tables_, options_.compaction_trigger,
                                &input_idxs)) {
    return Status::OK();
  }

  std::vector<Table*> inputs;
  std::vector<uint64_t> input_numbers;
  inputs.reserve(input_idxs.size());
  for (size_t idx : input_idxs) {
    inputs.push_back(tables_[idx].get());
    input_numbers.push_back(tables_[idx]->number());
  }

  const uint64_t out_number = next_file_number_++;
  const std::string out_fname = TableFileName(dbname_, out_number);

  lock.unlock();
  Status s = CompactTables(env_, out_fname, inputs);
  std::unique_ptr<Table> out_table;
  if (s.ok()) {
    s = Table::Open(env_, out_fname, &out_table);
    if (s.ok()) {
      out_table->set_number(out_number);
    }
  }
  lock.lock();

  if (!s.ok()) {
    env_->DeleteFile(out_fname).ok();
    return s;
  }

  std::set<uint64_t> drop(input_numbers.begin(), input_numbers.end());
  std::vector<uint64_t> new_table_numbers;
  for (const auto& t : tables_) {
    if (drop.count(t->number()) == 0) {
      new_table_numbers.push_back(t->number());
    }
  }
  new_table_numbers.push_back(out_number);
  s = PersistManifestWithTables(new_table_numbers, prev_log_number_);
  if (!s.ok()) {
    env_->DeleteFile(out_fname).ok();
    return s;
  }
  if (bg_hook_for_test_) bg_hook_for_test_("compaction_manifest_installed");

  std::vector<std::unique_ptr<Table>> kept;
  kept.reserve(tables_.size() - input_numbers.size() + 1);
  for (auto& t : tables_) {
    if (drop.count(t->number()) == 0) {
      kept.push_back(std::move(t));
    } else {
      const std::string old = TableFileName(dbname_, t->number());
      t.reset();
      env_->DeleteFile(old).ok();
    }
  }
  kept.push_back(std::move(out_table));
  tables_.swap(kept);

  env_->SyncDir(dbname_).ok();
  ++compaction_count_;
  return Status::OK();
}

Status DBImpl::MakeRoomForWrite(std::unique_lock<std::mutex>& lock) {
  while (true) {
    if (!bg_error_.ok()) {
      return bg_error_;
    }
    if (mem_->ApproximateMemoryUsage() < options_.write_buffer_size) {
      return Status::OK();
    }
    if (imm_ != nullptr) {
      MaybeScheduleBackgroundWork();
      cv_.wait(lock);
      continue;
    }
    const uint64_t new_log = next_file_number_++;
    Status s = RotateMemTableLog(new_log);
    if (!s.ok()) {
      return s;
    }
    imm_ = mem_;
    mem_ = new MemTable();
    mem_->Ref();
    MaybeScheduleBackgroundWork();
    return Status::OK();
  }
}

Status DBImpl::Write(const WriteOptions& options, WriteBatch* updates) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (closed_) {
    return Status::IOError("db is closed");
  }
  if (updates->Count() == 0) {
    return Status::OK();
  }

  Status s = MakeRoomForWrite(lock);
  if (!s.ok()) {
    return s;
  }

  const SequenceNumber seq = last_sequence_ + 1;
  updates->SetSequence(seq);

  s = log_->AddRecord(updates->Contents());
  if (!s.ok()) {
    return s;
  }
  if (options.sync.value_or(options_.sync)) {
    s = log_->Sync();
    if (!s.ok()) {
      return s;
    }
  }

  s = updates->InsertInto(mem_);
  if (!s.ok()) {
    return s;
  }
  last_sequence_ = seq + static_cast<SequenceNumber>(updates->Count()) - 1;
  return Status::OK();
}

Status DBImpl::Put(const WriteOptions& options, const Slice& key,
                   const Slice& value) {
  if (key.empty()) {
    return Status::InvalidArgument("empty key is not allowed");
  }
  WriteBatch batch;
  batch.Put(key, value);
  return Write(options, &batch);
}

Status DBImpl::Delete(const WriteOptions& options, const Slice& key) {
  if (key.empty()) {
    return Status::InvalidArgument("empty key is not allowed");
  }
  WriteBatch batch;
  batch.Delete(key);
  return Write(options, &batch);
}

Status DBImpl::GetFromTables(const Slice& user_key, std::string* value) {
  bool any_found = false;
  SequenceNumber best_sequence = 0;
  Status best_status = Status::NotFound(user_key.ToString());
  std::string best_value;
  for (const auto& table : tables_) {
    bool found = false;
    SequenceNumber sequence = 0;
    std::string candidate;
    Status s = table->Get(user_key, last_sequence_, &candidate, &found,
                          &sequence);
    if (!s.ok() && !s.IsNotFound()) {
      return s;
    }
    if (found && (!any_found || sequence > best_sequence)) {
      any_found = true;
      best_sequence = sequence;
      best_status = s;
      best_value = std::move(candidate);
    }
  }
  if (!any_found) return Status::NotFound(user_key.ToString());
  if (best_status.ok()) *value = std::move(best_value);
  return best_status.IsNotFound() ? Status::NotFound(user_key.ToString())
                                  : best_status;
}

Status DBImpl::Get(const ReadOptions& /*options*/, const Slice& key,
                   std::string* value) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (closed_) {
    return Status::IOError("db is closed");
  }

  LookupKey lkey(key, last_sequence_);
  Status s = mem_->Get(lkey, value);
  if (s.ok()) {
    return s;
  }
  if (s.IsNotFound() && s.message() == "deleted") {
    return Status::NotFound(key.ToString());
  }

  if (imm_ != nullptr) {
    s = imm_->Get(lkey, value);
    if (s.ok()) {
      return s;
    }
    if (s.IsNotFound() && s.message() == "deleted") {
      return Status::NotFound(key.ToString());
    }
  }

  return GetFromTables(key, value);
}

void DBImpl::WaitForBackgroundForTest() {
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    MaybeScheduleBackgroundWork();
    if (imm_ == nullptr && !bg_work_scheduled_ && !NeedCompactionLocked()) {
      break;
    }
    cv_.wait_for(lock, std::chrono::milliseconds(50));
  }
}

void DBImpl::SetBackgroundPausedForTest(bool paused) {
  std::lock_guard<std::mutex> lock(mutex_);
  bg_paused_for_test_ = paused;
  if (!paused) cv_.notify_all();
}

void DBImpl::SetBackgroundHookForTest(
    std::function<void(const char*)> hook) {
  std::lock_guard<std::mutex> lock(mutex_);
  bg_hook_for_test_ = std::move(hook);
}

size_t DBImpl::SstCountForTest() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return tables_.size();
}

uint64_t DBImpl::CompactionCountForTest() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return compaction_count_;
}

Status DBImpl::Close() {
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (closed_) {
      return Status::OK();
    }
    // The active MemTable remains protected by its WAL. Closing does not need
    // to turn a normal shutdown into a Flush transaction.
    MaybeScheduleBackgroundWork();
    while (bg_error_.ok() &&
           (imm_ != nullptr || bg_work_scheduled_ || NeedCompactionLocked())) {
      MaybeScheduleBackgroundWork();
      cv_.wait_for(lock, std::chrono::milliseconds(50));
    }

    shutting_down_ = true;
    cv_.notify_all();
  }

  if (bg_thread_.joinable()) {
    bg_thread_.join();
  }

  std::lock_guard<std::mutex> lock(mutex_);
  Status s = Status::OK();
  if (log_) {
    Status sync = log_->Sync();
    Status close = log_->Close();
    if (!sync.ok()) {
      s = sync;
    } else if (!close.ok()) {
      s = close;
    }
    log_.reset();
  }
  if (mem_) {
    mem_->Unref();
    mem_ = nullptr;
  }
  if (imm_) {
    imm_->Unref();
    imm_ = nullptr;
  }
  tables_.clear();
  closed_ = true;
  if (!s.ok()) {
    return s;
  }
  return bg_error_;
}

Status DB::Open(const Options& options, const std::string& name, DB** dbptr) {
  *dbptr = nullptr;
  DBImpl* impl = new DBImpl(options, name);
  Status s = impl->Open();
  if (!s.ok()) {
    delete impl;
    return s;
  }
  *dbptr = impl;
  return Status::OK();
}

}  // namespace minikv
