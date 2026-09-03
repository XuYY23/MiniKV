#pragma once

#include "env.h"
#include "minikv/db.h"
#include "minikv/status.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace minikv {
namespace bench {

inline uint64_t NowMicros() {
  using clock = std::chrono::steady_clock;
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          clock::now().time_since_epoch())
          .count());
}

inline std::string MakeKey(int i, int width = 8) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%0*d", width, i);
  return std::string(buf);
}

inline std::string MakeValue(int i, size_t size) {
  std::string v;
  v.resize(size, 'x');
  char head[32];
  std::snprintf(head, sizeof(head), "v%d-", i);
  const size_t n = std::min(size, std::strlen(head));
  for (size_t k = 0; k < n; ++k) {
    v[k] = head[k];
  }
  return v;
}

inline void CleanupEngineDir(const std::string& dbname) {
  Env* env = Env::Default();
  auto rm = [&](const std::string& p) {
    if (env->FileExists(p)) {
      env->DeleteFile(p).ok();
    }
  };
  rm(dbname + "/MANIFEST");
  rm(dbname + "/MANIFEST.tmp");
  for (int i = 1; i < 128; ++i) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s/%06d.log", dbname.c_str(), i);
    rm(buf);
    std::snprintf(buf, sizeof(buf), "%s/%06d.sst", dbname.c_str(), i);
    rm(buf);
  }
}

struct RunStats {
  std::string name;
  uint64_t ops = 0;
  uint64_t micros = 0;
  double ops_per_sec() const {
    if (micros == 0) {
      return 0;
    }
    return static_cast<double>(ops) * 1e6 / static_cast<double>(micros);
  }
};

inline void PrintStats(const RunStats& s) {
  std::printf("  %-28s  ops=%-8llu  time_us=%-10llu  thrpt=%.1f ops/s\n",
              s.name.c_str(),
              static_cast<unsigned long long>(s.ops),
              static_cast<unsigned long long>(s.micros),
              s.ops_per_sec());
}

// 基线 1：纯内存 map（无持久化）。
class MemoryMapStore {
 public:
  Status Put(const std::string& k, const std::string& v) {
    data_[k] = v;
    return Status::OK();
  }
  Status Delete(const std::string& k) {
    data_.erase(k);
    return Status::OK();
  }
  Status Get(const std::string& k, std::string* v) const {
    auto it = data_.find(k);
    if (it == data_.end()) {
      return Status::NotFound(k);
    }
    *v = it->second;
    return Status::OK();
  }

 private:
  std::map<std::string, std::string> data_;
};

// 基线 2：追加日志 + 内存 map；重启时整文件回放（极简 WAL）。
class SimpleWalMapStore {
 public:
  explicit SimpleWalMapStore(std::string path) : path_(std::move(path)) {}

  Status Open(bool create) {
    Env* env = Env::Default();
    if (env->FileExists(path_)) {
      Status s = Replay();
      if (!s.ok()) {
        return s;
      }
      return env->NewAppendableFile(path_, &file_);
    }
    if (!create) {
      return Status::InvalidArgument("missing simple wal file");
    }
    return env->NewWritableFile(path_, &file_);
  }

  Status Put(const std::string& k, const std::string& v) {
    Status s = AppendLine("P\t" + k + "\t" + v);
    if (!s.ok()) {
      return s;
    }
    data_[k] = v;
    return Status::OK();
  }

  Status Delete(const std::string& k) {
    Status s = AppendLine("D\t" + k);
    if (!s.ok()) {
      return s;
    }
    data_.erase(k);
    return Status::OK();
  }

  Status Get(const std::string& k, std::string* v) const {
    auto it = data_.find(k);
    if (it == data_.end()) {
      return Status::NotFound(k);
    }
    *v = it->second;
    return Status::OK();
  }

  Status Close() {
    if (!file_) {
      return Status::OK();
    }
    Status s = file_->Sync();
    Status c = file_->Close();
    file_.reset();
    return s.ok() ? c : s;
  }

 private:
  Status AppendLine(const std::string& line) {
    if (!file_) {
      return Status::IOError("wal not open");
    }
    std::string rec = line;
    rec.push_back('\n');
    return file_->Append(rec);
  }

  Status Replay() {
    std::unique_ptr<SequentialFile> in;
    Status s = Env::Default()->NewSequentialFile(path_, &in);
    if (!s.ok()) {
      return s;
    }
    std::string buf;
    char tmp[4096];
    while (true) {
      Slice frag;
      s = in->Read(sizeof(tmp), &frag, tmp);
      if (!s.ok()) {
        return s;
      }
      if (frag.empty()) {
        break;
      }
      buf.append(frag.data(), frag.size());
    }
    size_t start = 0;
    while (start < buf.size()) {
      size_t end = buf.find('\n', start);
      if (end == std::string::npos) {
        break;
      }
      const std::string line = buf.substr(start, end - start);
      start = end + 1;
      if (line.size() < 3) {
        continue;
      }
      if (line[0] == 'P') {
        const size_t t1 = line.find('\t', 2);
        if (t1 == std::string::npos) {
          continue;
        }
        data_[line.substr(2, t1 - 2)] = line.substr(t1 + 1);
      } else if (line[0] == 'D') {
        data_.erase(line.substr(2));
      }
    }
    return Status::OK();
  }

  std::string path_;
  std::map<std::string, std::string> data_;
  std::unique_ptr<WritableFile> file_;
};

}  // namespace bench
}  // namespace minikv
