#include "engine/version_edit.h"

#include "engine/filename.h"

#include <sstream>

namespace minikv {
namespace {

constexpr const char* kMagic = "MINIKV_MANIFEST_V1";

Status ReadFileToString(Env* env, const std::string& path, std::string* out) {
  std::unique_ptr<SequentialFile> file;
  Status s = env->NewSequentialFile(path, &file);
  if (!s.ok()) {
    return s;
  }
  out->clear();
  char buf[4096];
  while (true) {
    Slice fragment;
    s = file->Read(sizeof(buf), &fragment, buf);
    if (!s.ok()) {
      return s;
    }
    if (fragment.empty()) {
      break;
    }
    out->append(fragment.data(), fragment.size());
  }
  return Status::OK();
}

}  // namespace

Status SaveManifest(Env* env, const std::string& dbname,
                    const VersionEdit& edit) {
  const std::string tmp = TempFileName(dbname, "MANIFEST.tmp");
  const std::string final_path = ManifestFileName(dbname);

  std::ostringstream oss;
  oss << kMagic << "\n";
  oss << "next_file_number=" << edit.next_file_number << "\n";
  oss << "last_sequence=" << edit.last_sequence << "\n";
  oss << "log_number=" << edit.log_number << "\n";
  oss << "prev_log_number=" << edit.prev_log_number << "\n";
  for (uint64_t n : edit.sst_numbers) {
    oss << "sst=" << n << "\n";
  }
  const std::string body = oss.str();

  std::unique_ptr<WritableFile> file;
  Status s = env->NewWritableFile(tmp, &file);
  if (!s.ok()) {
    return s;
  }
  s = file->Append(body);
  if (!s.ok()) {
    return s;
  }
  s = file->Sync();
  if (!s.ok()) {
    return s;
  }
  s = file->Close();
  if (!s.ok()) {
    return s;
  }
  s = env->RenameFile(tmp, final_path);
  if (!s.ok()) {
    return s;
  }
  return env->SyncDir(dbname);
}

Status LoadManifest(Env* env, const std::string& dbname, VersionEdit* edit,
                    bool* found) {
  *found = false;
  edit->sst_numbers.clear();
  const std::string path = ManifestFileName(dbname);
  if (!env->FileExists(path)) {
    return Status::OK();
  }

  std::string content;
  Status s = ReadFileToString(env, path, &content);
  if (!s.ok()) {
    return s;
  }

  std::istringstream in(content);
  std::string line;
  if (!std::getline(in, line) || line != kMagic) {
    return Status::Corruption("bad MANIFEST magic");
  }
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    const auto eq = line.find('=');
    if (eq == std::string::npos) {
      return Status::Corruption("bad MANIFEST line");
    }
    const std::string key = line.substr(0, eq);
    const std::string val = line.substr(eq + 1);
    try {
      if (key == "next_file_number") {
        edit->next_file_number = std::stoull(val);
      } else if (key == "last_sequence") {
        edit->last_sequence = std::stoull(val);
      } else if (key == "log_number") {
        edit->log_number = std::stoull(val);
      } else if (key == "prev_log_number") {
        edit->prev_log_number = std::stoull(val);
      } else if (key == "sst") {
        edit->sst_numbers.push_back(std::stoull(val));
      } else {
        return Status::Corruption("unknown MANIFEST key");
      }
    } catch (...) {
      return Status::Corruption("bad MANIFEST number");
    }
  }
  *found = true;
  return Status::OK();
}

}  // namespace minikv
