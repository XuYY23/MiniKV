#include "engine/wal.h"
#include "engine/write_batch.h"
#include "env.h"
#include "minikv/slice.h"
#include "minikv/status.h"

#include <cctype>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>

namespace {

std::string Escape(const minikv::Slice& s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (size_t i = 0; i < s.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == '\\' || c == '"') {
      out.push_back('\\');
      out.push_back(static_cast<char>(c));
    } else if (std::isprint(c) && c != '\n' && c != '\r' && c != '\t') {
      out.push_back(static_cast<char>(c));
    } else {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\x%02x", c);
      out.append(buf);
    }
  }
  out.push_back('"');
  return out;
}

class PrintHandler : public minikv::WriteBatch::Handler {
 public:
  void Put(const minikv::Slice& key, const minikv::Slice& value) override {
    std::printf("    Put     %s = %s\n", Escape(key).c_str(),
                Escape(value).c_str());
    ++ops;
  }
  void Delete(const minikv::Slice& key) override {
    std::printf("    Delete  %s\n", Escape(key).c_str());
    ++ops;
  }
  int ops = 0;
};

void Usage(const char* argv0) {
  std::cerr << "用法: " << argv0 << " <wal_path>\n"
            << "  解码 MiniKV WAL（*.log），按记录打印 Put/Delete。\n"
            << "  示例: " << argv0 << " testdata/db_recover/000001.log\n"
            << "        " << argv0 << " ./data/demo/000001.log\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2 || std::string(argv[1]) == "-h" ||
      std::string(argv[1]) == "--help") {
    Usage(argv[0]);
    return argc == 2 ? 0 : 1;
  }

  const std::string path = argv[1];
  minikv::Env* env = minikv::Env::Default();
  if (!env->FileExists(path)) {
    std::cerr << "file not found: " << path << "\n";
    return 1;
  }

  std::unique_ptr<minikv::SequentialFile> file;
  minikv::Status s = env->NewSequentialFile(path, &file);
  if (!s.ok()) {
    std::cerr << s.ToString() << "\n";
    return 1;
  }

  std::printf("WAL dump: %s\n", path.c_str());
  std::printf("format: [checksum:4][length:4][WriteBatch payload]\n\n");

  minikv::WalReader reader(std::move(file));
  minikv::Slice payload;
  std::string scratch;
  int record_i = 0;
  int total_ops = 0;

  while (true) {
    const bool ok = reader.ReadRecord(&payload, &scratch, &s);
    if (!s.ok()) {
      std::cerr << "record #" << (record_i + 1) << " " << s.ToString() << "\n";
      return 1;
    }
    if (!ok) {
      break;
    }
    ++record_i;

    minikv::WriteBatch batch;
    batch.SetContents(payload);
    std::printf("#%-3d  payload_bytes=%zu  seq=%llu  count=%zu\n", record_i,
                payload.size(),
                static_cast<unsigned long long>(batch.Sequence()),
                batch.Count());

    PrintHandler handler;
    s = batch.Iterate(&handler);
    if (!s.ok()) {
      std::cerr << "  decode failed: " << s.ToString() << "\n";
      return 1;
    }
    total_ops += handler.ops;
    std::printf("\n");
  }

  std::printf("summary: records=%d ops=%d\n", record_i, total_ops);
  return 0;
}
