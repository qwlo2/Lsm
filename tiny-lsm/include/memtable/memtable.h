#pragma once

#include "iterator/iterator.h"

#include "skiplist/skiplist.h"
//#include "common/common.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

namespace tiny_lsm {

class BlockCache;
class SST;
class SSTBuilder;
class TranContext;
class LSMEngine;
class LSM;
//刷盘或者对外暴露数据时，要让所有数据有效且有序（合并操作），因此用最小堆维护
//memtable实际存储的是commit entry，因此不能更改，在wal中已经写入，每个事务都要直接的缓冲区
class MemTable {
  friend class TranContext;
  friend class HeapIterator;
  friend  class LSMEngine;
  friend class LSM;
private:
  void put_(const std::string &key, const std::string &value,
            uint64_t tranc_id,
            EntryState state = EntryState::COMMITTED,
            std::shared_ptr<SharedEntryState> shared_state = nullptr);

  SkipListIterator get_(
      const std::string &key, uint64_t tranc_id,
      ReadVisibility visibility = ReadVisibility::COMMITTED_ONLY);

  SkipListIterator cur_get_(const std::string &key, uint64_t tranc_id,
                            ReadVisibility visibility);

  SkipListIterator frozen_get_(const std::string &key, uint64_t tranc_id,
                               ReadVisibility visibility);

  void remove_(const std::string &key, uint64_t tranc_id,
               EntryState state = EntryState::COMMITTED,
               std::shared_ptr<SharedEntryState> shared_state = nullptr);
  void frozen_cur_table_(); // _ 表示不需要锁的版本
   //为了写冲突，标志与put_一致加的
   void maybe_frozen_cur_table_();
public:
  MemTable();
  ~MemTable();

  void put(const std::string &key, const std::string &value, uint64_t tranc_id,
           EntryState state = EntryState::COMMITTED,
           std::shared_ptr<SharedEntryState> shared_state = nullptr);
  void put_batch(const std::vector<std::pair<std::string, std::string>> &kvs,
                 uint64_t tranc_id,
                 EntryState state = EntryState::COMMITTED,
                 std::shared_ptr<SharedEntryState> shared_state = nullptr);

  SkipListIterator get(
      const std::string &key, uint64_t tranc_id,
      ReadVisibility visibility = ReadVisibility::COMMITTED_ONLY);
  std::vector<
      std::pair<std::string, std::optional<std::pair<std::string, uint64_t>>>>
  get_batch(const std::vector<std::string> &keys, uint64_t tranc_id,
            ReadVisibility visibility = ReadVisibility::COMMITTED_ONLY);
  void remove(const std::string &key, uint64_t tranc_id,
              EntryState state = EntryState::COMMITTED,
              std::shared_ptr<SharedEntryState> shared_state = nullptr);
  void remove_batch(const std::vector<std::string> &keys, uint64_t tranc_id,
                    EntryState state = EntryState::COMMITTED,
                    std::shared_ptr<SharedEntryState> shared_state = nullptr);

  void clear();
  std::shared_ptr<SST> flush_last(SSTBuilder &builder, std::string &sst_path,
                                  size_t sst_id,
                                  std::vector<uint64_t> &flushed_tranc_ids,
                                  std::shared_ptr<BlockCache> block_cache,
                                  std::shared_ptr<SkipList> &flushed_table,
                                  uint64_t watermark);
  void remove_flushed_table_(const std::shared_ptr<SkipList> &table);
  void frozen_cur_table();
  size_t get_cur_size();
  size_t get_frozen_size();
  size_t get_total_size();
  std::shared_ptr<HeapIterator>
  begin(uint64_t tranc_id,
        ReadVisibility visibility = ReadVisibility::COMMITTED_ONLY);
  HeapIterator
  iters_preffix(const std::string &preffix, uint64_t tranc_id,
                ReadVisibility visibility = ReadVisibility::COMMITTED_ONLY);

  std::optional<std::pair<HeapIterator, HeapIterator>>
  iters_monotony_predicate(uint64_t tranc_id,
                           std::function<int(const std::string &)> predicate,
                           ReadVisibility visibility =
                               ReadVisibility::COMMITTED_ONLY);

  HeapIterator end();

private:
  std::shared_ptr<SkipList> current_table;
  std::list<std::shared_ptr<SkipList>> frozen_tables;
  size_t frozen_bytes;
  std::shared_mutex frozen_mtx; // 冻结表的锁
  std::shared_mutex cur_mtx;    // 活跃表的锁
};
} // namespace tiny_lsm
