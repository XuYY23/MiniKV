#include "engine/memtable.h"
#include "tests/testharness.h"

int main() {
  minikv::test::LogSection("TEST SUITE: test_memtable (小节 S3)");

  auto* mem = new minikv::MemTable();
  mem->Ref();

  minikv::test::LogStep("Add a=va seq=1, b=vb seq=2, delete a seq=3");
  mem->Add(1, minikv::kTypeValue, "a", "va");
  mem->Add(2, minikv::kTypeValue, "b", "vb");
  mem->Add(3, minikv::kTypeDeletion, "a", "");

  std::string value;
  {
    minikv::test::LogStep("Get(b) should return vb");
    minikv::LookupKey lk("b", 100);
    CHECK_OK(mem->Get(lk, &value));
    CHECK_EQ(value, std::string("vb"));
  }
  {
    minikv::test::LogStep("Get(a) after deletion should be NotFound(deleted)");
    minikv::LookupKey lk("a", 100);
    minikv::Status s = mem->Get(lk, &value);
    CHECK(s.IsNotFound());
    CHECK_EQ(s.message(), std::string("deleted"));
  }
  {
    minikv::test::LogStep("Get(c) missing key should be NotFound");
    minikv::LookupKey lk("c", 100);
    CHECK(mem->Get(lk, &value).IsNotFound());
  }

  mem->Unref();
  return minikv::test::Report("test_memtable");
}
