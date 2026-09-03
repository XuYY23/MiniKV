#include "engine/compaction.h"

#include "engine/dbformat.h"
#include "engine/filename.h"

#include <algorithm>
#include <functional>
#include <map>
#include <queue>

namespace minikv {

int SizeTierBucket(uint64_t file_size) {
  if (file_size < 1024) {
    return 0;
  }
  int tier = 0;
  uint64_t x = file_size;
  while (x >= 2048) {
    x >>= 1;
    ++tier;
  }
  return tier;
}

bool PickSizeTieredCompaction(const std::vector<std::unique_ptr<Table>>& tables,
                              size_t trigger,
                              std::vector<size_t>* inputs) {
  inputs->clear();
  if (tables.size() < trigger) {
    return false;
  }

  std::map<int, std::vector<size_t>> buckets;
  for (size_t i = 0; i < tables.size(); ++i) {
    buckets[SizeTierBucket(tables[i]->file_size())].push_back(i);
  }

  for (auto& kv : buckets) {
    auto& idxs = kv.second;
    if (idxs.size() < trigger) {
      continue;
    }
    std::sort(idxs.begin(), idxs.end(), [&](size_t a, size_t b) {
      return tables[a]->number() < tables[b]->number();
    });
    inputs->assign(idxs.begin(),
                   idxs.begin() + static_cast<std::ptrdiff_t>(trigger));
    return true;
  }
  return false;
}

namespace {

struct HeapItem {
  Table::Iterator* it = nullptr;
  bool operator>(const HeapItem& o) const {
    return CompareInternalKey(it->key(), o.it->key()) > 0;
  }
};

}  // namespace

Status CompactTables(Env* env, const std::string& output_fname,
                     const std::vector<Table*>& inputs) {
  std::vector<std::unique_ptr<Table::Iterator>> owned;
  std::priority_queue<HeapItem, std::vector<HeapItem>, std::greater<HeapItem>>
      heap;

  for (Table* t : inputs) {
    auto it = std::unique_ptr<Table::Iterator>(t->NewIterator());
    it->SeekToFirst();
    if (!it->status().ok()) {
      return it->status();
    }
    if (it->Valid()) {
      Table::Iterator* raw = it.get();
      owned.push_back(std::move(it));
      heap.push(HeapItem{raw});
    }
  }

  TableBuilder builder(env, output_fname);
  std::string last_user_key;
  bool has_last = false;

  while (!heap.empty()) {
    HeapItem top = heap.top();
    heap.pop();
    if (!top.it->status().ok()) {
      builder.Abandon().ok();
      return top.it->status();
    }

    const Slice ikey = top.it->key();
    const Slice ukey = ExtractUserKey(ikey);
    const bool drop =
        has_last && ukey.compare(Slice(last_user_key)) == 0;
    if (!drop) {
      builder.Add(ikey, top.it->value());
      last_user_key.assign(ukey.data(), ukey.size());
      has_last = true;
    }

    top.it->Next();
    if (!top.it->status().ok()) {
      builder.Abandon().ok();
      return top.it->status();
    }
    if (top.it->Valid()) {
      heap.push(top);
    }
  }

  if (builder.NumEntries() == 0) {
    return builder.Abandon();
  }
  return builder.Finish();
}

}  // namespace minikv
