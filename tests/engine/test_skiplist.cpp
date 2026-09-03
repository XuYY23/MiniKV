#include "engine/arena.h"
#include "engine/skiplist.h"
#include "tests/testharness.h"

#include <set>
#include <string>

namespace {

struct Cmp {
  int operator()(const std::string& a, const std::string& b) const {
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
  }
};

void TestEmpty() {
  minikv::test::LogSection("S2 SkipList / empty");
  minikv::Arena arena;
  minikv::SkipList<std::string, Cmp> list(Cmp(), &arena);

  minikv::test::LogStep("empty list should not contain key \"x\"");
  CHECK(!list.Contains("x"));

  minikv::SkipList<std::string, Cmp>::Iterator it(&list);
  it.SeekToFirst();
  minikv::test::LogStep("iterator SeekToFirst on empty list");
  CHECK(!it.Valid());
}

void TestInsertAndSeek() {
  minikv::test::LogSection("S2 SkipList / insert-seek-iterate");
  minikv::Arena arena;
  minikv::SkipList<std::string, Cmp> list(Cmp(), &arena);
  std::set<std::string> model;

  minikv::test::LogStep("insert up to 1000 keys (dedup via set model)");
  for (int i = 0; i < 1000; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%06d", i * 3 % 997);
    const std::string key(buf);
    if (model.insert(key).second) {
      list.Insert(key);
    }
  }
  minikv::test::Log("  model size = " + std::to_string(model.size()));

  minikv::test::LogStep("every model key must be Contains()==true");
  for (const auto& key : model) {
    CHECK(list.Contains(key));
  }

  minikv::test::LogStep("missing key must be absent");
  CHECK(!list.Contains("missing"));

  minikv::test::LogStep("in-order iteration must match std::set");
  minikv::SkipList<std::string, Cmp>::Iterator it(&list);
  it.SeekToFirst();
  for (const auto& key : model) {
    CHECK(it.Valid());
    CHECK_EQ(it.key(), key);
    it.Next();
  }
  CHECK(!it.Valid());
}

}  // namespace

int main() {
  minikv::test::LogSection("TEST SUITE: test_skiplist (小节 S2)");
  TestEmpty();
  TestInsertAndSeek();
  return minikv::test::Report("test_skiplist");
}
