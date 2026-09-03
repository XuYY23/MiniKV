#include "engine/filename.h"

#include <cstdio>

namespace minikv {

std::string LogFileName(const std::string& dbname, uint64_t number) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%06llu.log",
                static_cast<unsigned long long>(number));
  return dbname + "/" + buf;
}

std::string TableFileName(const std::string& dbname, uint64_t number) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%06llu.sst",
                static_cast<unsigned long long>(number));
  return dbname + "/" + buf;
}

std::string ManifestFileName(const std::string& dbname) {
  return dbname + "/MANIFEST";
}

std::string TempFileName(const std::string& dbname, const std::string& suffix) {
  return dbname + "/" + suffix;
}

bool ParseFileNumber(const std::string& fname, uint64_t* number) {
  if (fname.size() < 10) {
    return false;
  }
  unsigned long long n = 0;
  if (std::sscanf(fname.c_str(), "%llu", &n) != 1) {
    return false;
  }
  *number = static_cast<uint64_t>(n);
  return true;
}

}  // namespace minikv
