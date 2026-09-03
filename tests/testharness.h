#pragma once

#include "env.h"
#include "minikv/status.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

namespace minikv {
namespace test {

inline int& FailureCount() {
  static int n = 0;
  return n;
}

inline void Log(const std::string& msg) {
  std::cout << msg << std::endl;
}

inline void LogSection(const std::string& title) {
  std::cout << "\n==== " << title << " ====\n";
}

inline void LogStep(const std::string& step) {
  std::cout << "[STEP] " << step << std::endl;
}

inline void Check(bool cond, const char* expr, const char* file, int line) {
  if (cond) {
    std::cout << "  [OK]   " << expr << std::endl;
  } else {
    std::cerr << "  [FAIL] " << file << ":" << line << "  " << expr << std::endl;
    ++FailureCount();
  }
}

template <typename A, typename B>
inline void CheckEq(const A& a, const B& b, const char* a_expr, const char* b_expr,
                    const char* file, int line, bool verbose = true) {
  std::ostringstream oa;
  std::ostringstream ob;
  oa << a;
  ob << b;
  if (a == b) {
    if (verbose) {
      std::cout << "  [OK]   " << a_expr << " == " << b_expr << "  (value=\""
                << oa.str() << "\")\n";
    }
  } else {
    std::cerr << "  [FAIL] " << file << ":" << line << "  " << a_expr
              << " == " << b_expr << "\n         expected: \"" << ob.str()
              << "\"\n"
              << "         actual:   \"" << oa.str() << "\"\n";
    ++FailureCount();
  }
}

inline void CheckOk(const Status& s, const char* expr, const char* file, int line,
                    bool verbose = true) {
  if (s.ok()) {
    if (verbose) {
      std::cout << "  [OK]   " << expr << " -> OK\n";
    }
  } else {
    std::cerr << "  [FAIL] " << file << ":" << line << "  " << expr
              << " -> " << s.ToString() << "\n";
    ++FailureCount();
  }
}

inline int Report(const std::string& suite_name) {
  std::cout << "\n---- " << suite_name << " summary ----\n";
  if (FailureCount() == 0) {
    std::cout << "RESULT: ALL PASSED\n";
    return 0;
  }
  std::cout << "RESULT: FAILED (" << FailureCount() << " check(s))\n";
  return 1;
}

// 在当前工作目录下创建相对路径测试目录（不依赖 /tmp 或绝对路径）。
// CTest 已将工作目录设为 build/，因此数据落在 build/testdata/<name>/。
inline std::string MakeTestDir(const std::string& name) {
  Env* env = Env::Default();
  const std::string root = "testdata";
  if (!env->FileExists(root)) {
    Status s = env->CreateDir(root);
    if (!s.ok() && !env->FileExists(root)) {
      std::cerr << "MakeTestDir: cannot create " << root << ": " << s.ToString()
                << "\n";
      std::exit(1);
    }
  }
  const std::string path = root + "/" + name;
  if (!env->FileExists(path)) {
    Status s = env->CreateDir(path);
    if (!s.ok() && !env->FileExists(path)) {
      std::cerr << "MakeTestDir: cannot create " << path << ": " << s.ToString()
                << "\n";
      std::exit(1);
    }
  }
  LogStep("test work dir = " + path + " (relative to process cwd)");
  return path;
}

inline void RemoveFileIfExists(const std::string& path) {
  Env* env = Env::Default();
  if (env->FileExists(path)) {
    Status s = env->DeleteFile(path);
    if (!s.ok()) {
      std::cerr << "RemoveFileIfExists: " << path << " " << s.ToString() << "\n";
    }
  }
}

}  // namespace test
}  // namespace minikv

#define CHECK(x) \
  ::minikv::test::Check(!!(x), #x, __FILE__, __LINE__)

#define CHECK_EQ(a, b) \
  ::minikv::test::CheckEq((a), (b), #a, #b, __FILE__, __LINE__, true)

#define CHECK_EQ_QUIET(a, b) \
  ::minikv::test::CheckEq((a), (b), #a, #b, __FILE__, __LINE__, false)

#define CHECK_OK(s) \
  ::minikv::test::CheckOk((s), #s, __FILE__, __LINE__, true)

#define CHECK_OK_QUIET(s) \
  ::minikv::test::CheckOk((s), #s, __FILE__, __LINE__, false)
