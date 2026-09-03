#include "minikv/db.h"

#include <iostream>
#include <sstream>
#include <string>

namespace {

void PrintUsage(const char* argv0) {
  std::cerr << "用法:\n"
            << "  " << argv0 << " <db_path> put <key> <value>\n"
            << "  " << argv0 << " <db_path> get <key>\n"
            << "  " << argv0 << " <db_path> delete <key>\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    PrintUsage(argv[0]);
    return 1;
  }

  const std::string dbpath = argv[1];
  const std::string cmd = argv[2];

  minikv::Options options;
  options.create_if_missing = true;
  minikv::DB* db = nullptr;
  minikv::Status s = minikv::DB::Open(options, dbpath, &db);
  if (!s.ok()) {
    std::cerr << s.ToString() << "\n";
    return 1;
  }

  minikv::WriteOptions wopt;
  minikv::ReadOptions ropt;
  int code = 0;

  if (cmd == "put") {
    if (argc != 5) {
      PrintUsage(argv[0]);
      code = 1;
    } else {
      s = db->Put(wopt, argv[3], argv[4]);
      if (!s.ok()) {
        std::cerr << s.ToString() << "\n";
        code = 1;
      } else {
        std::cout << "OK\n";
      }
    }
  } else if (cmd == "get") {
    if (argc != 4) {
      PrintUsage(argv[0]);
      code = 1;
    } else {
      std::string value;
      s = db->Get(ropt, argv[3], &value);
      if (s.IsNotFound()) {
        std::cout << "(null)\n";
      } else if (!s.ok()) {
        std::cerr << s.ToString() << "\n";
        code = 1;
      } else {
        std::cout << value << "\n";
      }
    }
  } else if (cmd == "delete") {
    if (argc != 4) {
      PrintUsage(argv[0]);
      code = 1;
    } else {
      s = db->Delete(wopt, argv[3]);
      if (!s.ok()) {
        std::cerr << s.ToString() << "\n";
        code = 1;
      } else {
        std::cout << "OK\n";
      }
    }
  } else {
    PrintUsage(argv[0]);
    code = 1;
  }

  db->Close().ok();
  delete db;
  return code;
}
