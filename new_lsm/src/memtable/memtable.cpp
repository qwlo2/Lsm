#include "memtable/memtable.h"
#include "config/config.h"
#include "consts.h"
#include "iterator/iterator.h"
#include "skiplist/skiplist.h"
#include "sst/sst.h"
#include "spdlog/spdlog.h"
#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace tiny_lsm {

class BlockCache;

// MemTable implementation using PIMPL idiom
MemTable::MemTable() : frozen_bytes(0) {
  current_table = std::make_shared<SkipList>();
}
MemTable::~MemTable() = default;
// TODO: Lab2.1 无锁版本的 put
  // ? 直接调用 current_table 的 put 方法
void MemTable::put_(const std::string &key, const std::string &value,
                    uint64_t tranc_id) {
        current_table->put(key, value, tranc_id);
}
// TODO: Lab2.1 有锁版本的 put
  // ? 加 cur_mtx 写锁后调用 put_()
  // ? 若 current_table 超过 LsmPerMemSizeLimit, 还需加 frozen_mtx 写锁并调用 frozen_cur_table_()
void MemTable::put(const std::string &key, const std::string &value,
                   uint64_t tranc_id) {
  std::unique_lock<std::shared_mutex> lock(cur_mtx);
  put_(key, value, tranc_id);
  lock.unlock();
  //get_cur_size()里面对current_table加了读锁
  if (get_cur_size()>=TomlConfig::getInstance().getLsmPerMemSizeLimit()) {
    frozen_cur_table();
  }

}
// TODO: Lab2.1 有锁版本的 put_batch
  // ? 加 cur_mtx 写锁后遍历 kvs 依次调用 put_()
  // ? 结束后若超限则冻结当前表
void MemTable::put_batch(
    const std::vector<std::pair<std::string, std::string>> &kvs,
    uint64_t tranc_id) {
      if (kvs.empty()) {
         return;
      }
  std::unique_lock<std::shared_mutex> lock(cur_mtx);
  for (const auto &kv : kvs) {
    put_(kv.first, kv.second, tranc_id);
  }
  lock.unlock();
  if (get_cur_size()>= TomlConfig::getInstance().getLsmPerMemSizeLimit()) {
    frozen_cur_table();
   }
}
// 检查当前活跃的memtable
  // TODO: Lab2.1 从活跃跳表中查询
  // ? 调用 current_table->get(), 找到则返回; 未找到则返回空迭代器
SkipListIterator MemTable::cur_get_(const std::string &key, uint64_t tranc_id) {
   if (current_table) {
      if (auto ans=current_table.get()->get(key, tranc_id);ans.is_valid()) {
        return ans;
      }
   }
  return SkipListIterator{};
}
// TODO: Lab2.1 从冻结跳表中查询
  // ? 遍历 frozen_tables (注意顺序：越靠前越新), 找到即返回
  // ? tranc_id 直接传递到 get() 即可
SkipListIterator MemTable::frozen_get_(const std::string &key,
                                       uint64_t tranc_id) {
     for (auto& it : frozen_tables) {
          if (auto ans=it->get(key, tranc_id);ans.is_valid()) {
            return ans;
          }
     }
  return SkipListIterator{};
}
// TODO: Lab2.1 查询, 建议复用 cur_get_ 和 frozen_get_
  // ? 先加 cur_mtx 读锁查活跃表, 未命中则释放锁后加 frozen_mtx 读锁查冻结表
SkipListIterator MemTable::get(const std::string &key, uint64_t tranc_id) {
  std::shared_lock<std::shared_mutex> lock(cur_mtx);
  if ( auto ans=cur_get_(key, tranc_id); ans.is_valid()) {
    return ans;
  } else {
    lock.unlock();
    std::shared_lock<std::shared_mutex> frozen_lock(frozen_mtx);
    if (auto ans=frozen_get_(key, tranc_id); ans.is_valid()) {
      return ans;
    }
  }
  return SkipListIterator{};
}

SkipListIterator MemTable::get_(const std::string &key, uint64_t tranc_id) {
  // TODO: Lab2.1 查询, 无锁版本
  // ? 直接调用 cur_get_ 和 frozen_get_
  if (auto ans = cur_get_(key, tranc_id); ans.is_valid()) {
    return ans;
  }else if (auto ans=frozen_get_(key, tranc_id); ans.is_valid()) {
    return ans;
  }
  return SkipListIterator{};
}
//迭代器无法处理未找到这种情况
std::vector<
    std::pair<std::string, std::optional<std::pair<std::string, uint64_t>>>>
MemTable::get_batch(const std::vector<std::string> &keys, uint64_t tranc_id) {
  spdlog::trace("MemTable--get_batch with {} keys", keys.size());

  std::vector<
      std::pair<std::string, std::optional<std::pair<std::string, uint64_t>>>>
      results;
  results.reserve(keys.size());

  // 1. 先获取活跃表的锁
  std::shared_lock<std::shared_mutex> slock1(cur_mtx);
  for (size_t idx = 0; idx < keys.size(); idx++) {
    auto key = keys[idx];
    auto cur_res = cur_get_(key, tranc_id);
    if (cur_res.is_valid()) {
      // 值存在且不为空
      results.emplace_back(
          key, std::make_pair(cur_res.get_value(), cur_res.get_tranc_id()));
    } else {
      // 如果活跃表中未找到，先占位
      results.emplace_back(key, std::nullopt);
    }
  }

  // 2. 如果某些键在活跃表中未找到，还需要查找冻结表
  if (!std::any_of(results.begin(), results.end(), [](const auto &result) {
        return !result.second.has_value();
      })) {
    return results;
  }

  slock1.unlock(); // 释放活跃表的锁
  std::shared_lock<std::shared_mutex> slock2(frozen_mtx); // 获取冻结表的锁
  for (size_t idx = 0; idx < keys.size(); idx++) {
    if (results[idx].second.has_value()) {
      continue; // 如果在活跃表中已经找到，则跳过
    }
    auto key = keys[idx];
    auto frozen_result = frozen_get_(key, tranc_id);
    if (frozen_result.is_valid()) {
      // 值存在且不为空
      results[idx] =
          std::make_pair(key, std::make_pair(frozen_result.get_value(),
                                             frozen_result.get_tranc_id()));
    } else {
      results[idx] = std::make_pair(key, std::nullopt);
    }
  }

  return results;
}
// TODO: Lab2.1 无锁版本的remove
  // ? 在 LSM 中, 删除操作是写入空值, 调用 current_table->put(key, "", tranc_id)
void MemTable::remove_(const std::string &key, uint64_t tranc_id) {
  current_table->put(key, "", tranc_id);

}

void MemTable::remove(const std::string &key, uint64_t tranc_id) {
  // TODO: Lab2.1 有锁版本的remove
  // ? 加 cur_mtx 写锁后调用 remove_()
  // ? 若超限则冻结当前表
  std::unique_lock<std::shared_mutex> lock(cur_mtx);
  remove_(key, tranc_id);
  
}
 // TODO: Lab2.1 有锁版本的remove_batch
  // ? 加 cur_mtx 写锁后遍历 keys 依次调用 remove_()
  // ? 结束后若超限则冻结当前表
void MemTable::remove_batch(const std::vector<std::string> &keys,
                            uint64_t tranc_id) {
  std::unique_lock<std::shared_mutex> lock(cur_mtx);
  for (const auto &key : keys) {
    remove_(key, tranc_id);
  }
}

void MemTable::clear() {
  spdlog::info("MemTable--clear(): Clearing all tables");

  std::unique_lock<std::shared_mutex> lock1(cur_mtx);
  std::unique_lock<std::shared_mutex> lock2(frozen_mtx);
  frozen_tables.clear();
  current_table->clear();
}

// 将最老的 memtable 写入 SST, 并返回控制类
std::shared_ptr<SST>
MemTable::flush_last(SSTBuilder &builder, std::string &sst_path, size_t sst_id,
                     std::vector<uint64_t> &flushed_tranc_ids,
                     std::shared_ptr<BlockCache> block_cache,
                    std::shared_ptr<SkipList> &flushed_table,
                    uint64_t watermark) {
  spdlog::debug("MemTable--flush_last(): Starting to flush memtable to SST{}",
                sst_id);
std::shared_ptr<SkipList> table;
uint64_t min_tranc_id = UINT64_MAX;
 uint64_t max_tranc_id = 0;
{ 
    // 由于 flush 后需要移除最老的 memtable, 因此需要加写锁
  std::unique_lock<std::shared_mutex> lock(frozen_mtx);
  if (frozen_tables.empty()) {
        return nullptr;
   }
  // 将最老的 memtable 写入 SST
  //  table = frozen_tables.back();
  // frozen_tables.pop_back();
  // frozen_bytes -= table->get_size();
   table = frozen_tables.back();
   flushed_table = table;
  }
  // 这样会有一个短时间窗口，table 已经从 frozen_tables 移走但 SST
  // 还没发布，严格并发读会看不到它。正确工业做法是加一个 flushing_tables
  // 列表，读 memtable 时也查它；SST build 完成并发布后，再从 flushing_tables 移除。
  //暂不实现

 //std::cerr << "before skiplist flush\n";
  auto flush_data =table->flush();
    //std::cerr << "after skiplist flush size=" << flush_data.size() << "\n";
      //过滤
      std::string cur_key;
      bool kept_le_watermark = false;
    //  std::cerr << "before builder add\n";
      for (auto &[k, v, t] : flush_data) {
        if (k == "" && v == "") {
          flushed_tranc_ids.push_back(t);
           continue;
        }
       // std::cerr << "builder add done\n";
        max_tranc_id = (std::max)(t, max_tranc_id);
        min_tranc_id = (std::min)(t, min_tranc_id);
        if (k != cur_key) {
          cur_key = k;
          kept_le_watermark = false;
        }

        bool keep = false;

        if (t > watermark) {
          keep = true;
        } else if (!kept_le_watermark) {
          kept_le_watermark = true;
          keep = true;
        }

        if (keep) {
          builder.add(k, v, t);
        }
        //std::cerr << "builder build done\n";
      }
     // std::cerr << "after builder add\n";
      //std::cerr << "before builder build\n";
  auto sst = builder.build(sst_id, sst_path, block_cache);
//std::cerr << "after builder build\n";
  spdlog::info("MemTable--flush_last(): SST{} built successfully at '{}'",
               sst_id, sst_path);

  return sst;
}
void MemTable::remove_flushed_table_(const std::shared_ptr<SkipList> &table) {
  std::unique_lock<std::shared_mutex> lock(frozen_mtx);

  for (auto it = frozen_tables.begin(); it != frozen_tables.end(); ++it) {
    if (*it == table) {
      frozen_bytes -= table->get_size();
      frozen_tables.erase(it);
      return;
    }
  }
}
//
void MemTable::maybe_frozen_cur_table_() {
    if (current_table->get_size() >= TomlConfig::getInstance().getLsmPerMemSizeLimit()) {
      //std::unique_lock lock(frozen_mtx);
        frozen_cur_table_();
    }
}
// TODO: Lab2.1 冻结活跃表（无锁版本）
  // ? 将 current_table 移入 frozen_tables 头部, 并更新 frozen_bytes
  // ? 创建新的空 SkipList 作为 current_table
void MemTable::frozen_cur_table_() {
       
       frozen_tables.emplace_front(
         current_table
       );
       frozen_bytes += current_table->get_size();
       current_table = std::make_shared<SkipList>();
}
// TODO: Lab2.1 冻结活跃表（有锁版本）
  // ? 加 cur_mtx 和 frozen_mtx 写锁后调用 frozen_cur_table_()
void MemTable::frozen_cur_table() {
  std::unique_lock<std::shared_mutex> cur_lock(cur_mtx);
  std::unique_lock<std::shared_mutex> frozen_lock(frozen_mtx);
  frozen_cur_table_();
}

size_t MemTable::get_cur_size() {
  std::shared_lock<std::shared_mutex> slock(cur_mtx);
  return current_table->get_size();
}

size_t MemTable::get_frozen_size() {
  std::shared_lock<std::shared_mutex> slock(frozen_mtx);
  return frozen_bytes;
}

size_t MemTable::get_total_size() {
  std::shared_lock<std::shared_mutex> slock1(cur_mtx);
  std::shared_lock<std::shared_mutex> slock2(frozen_mtx);
  return frozen_bytes + current_table->get_size();
}

// TODO: 需要进一步判断这里的 HeapIterator 能否跳过删除元素
std::shared_ptr<HeapIterator> MemTable::begin(uint64_t tranc_id) {
  // TODO: Lab2.2 MemTable 的迭代器
  // ? 加 cur_mtx 和 frozen_mtx 读锁, 遍历所有表收集 SearchItem
  // ? 每个 item 包含 key, value, table_idx, 0, tranc_id
  // ? 过滤 tranc_id 不可见的记录 (tranc_id != 0 && iter.get_tranc_id() > tranc_id)
  // ? 返回 HeapIterator(item_vec, tranc_id)
   std::shared_lock<std::shared_mutex> cur_lock(cur_mtx);
     std::shared_lock<std::shared_mutex> frozen_lock(frozen_mtx);
     
        std::vector<SearchItem> item_vec;
        int idx = 0;
        for (auto it=current_table->begin(); it.is_valid(); ++it) {
             if (tranc_id != 0 && it.get_tranc_id() > tranc_id) {
                     continue;
             }
           item_vec.emplace_back(it.get_key(), it.get_value(), 0, 0, it.get_tranc_id());     
        }
        for (auto& it:frozen_tables) {
          idx++;
          for (auto it2=it->begin(); it2.is_valid(); ++it2) {
            if (tranc_id != 0 && it2.get_tranc_id() > tranc_id) {
              continue;
            }
            item_vec.emplace_back(it2.get_key(), it2.get_value(), idx, 0, it2.get_tranc_id());
          }
        }
        return std::make_shared<HeapIterator>(HeapIterator(item_vec, tranc_id));
}

HeapIterator MemTable::end() {
  // TODO: Lab2.2 MemTable 的迭代器
  // ? 加读锁后返回空 HeapIterator

  return HeapIterator{};
}

HeapIterator MemTable::iters_preffix(const std::string &preffix,
                                     uint64_t tranc_id) {
  // TODO: Lab2.3 MemTable 的前缀迭代器
  // ? 加读锁, 对所有表调用 begin_preffix/end_preffix 遍历前缀范围
  // ? 过滤事务可见性, 同 key 只保留最新版本
     std::shared_lock<std::shared_mutex> cur_lock(cur_mtx);
   std::shared_lock<std::shared_mutex> frozen_lock(frozen_mtx);
   
      int idx=0;
      std::vector<SearchItem> tem;
       auto  lowwer_boundry=current_table->begin_preffix(preffix);
       auto  upper_boundry=current_table->end_preffix(preffix);
       auto lam=[&tem,&upper_boundry,&lowwer_boundry,&idx](){
            if (lowwer_boundry!=upper_boundry) {
                 for (auto& its=lowwer_boundry;its!=upper_boundry;++its) {
                            tem.emplace_back(its.get_key(),its.get_value(),idx,0,its.get_tranc_id());
                 }
              }
      };
        lam();
        for (auto& it : frozen_tables) {
                lowwer_boundry=it->begin_preffix(preffix);
                upper_boundry=it->end_preffix(preffix);
                idx++;
                 lam();
             }
        return  HeapIterator(tem,tranc_id);
}

std::optional<std::pair<HeapIterator, HeapIterator>>
MemTable::iters_monotony_predicate(
    uint64_t tranc_id, std::function<int(const std::string &)> predicate) {
  // TODO: Lab2.3 MemTable 的谓词查询迭代器起始范围
  // ? 加读锁, 对所有表调用 iters_monotony_predicate 获取结果
  // ? 过滤事务可见性, 同 key 只保留最新版本
  // ? 若结果为空返回 nullopt; 否则返回 make_pair(HeapIterator(item_vec, tranc_id, true), HeapIterator{})
      std::shared_lock<std::shared_mutex> cur_lock(cur_mtx);
   std::shared_lock<std::shared_mutex> frozen_lock(frozen_mtx);
  
      int idx=0;
      std::vector<SearchItem> tem;
      auto la=current_table->iters_monotony_predicate(predicate);
      auto lam=[&tem,&la,&idx](){
         auto  lowwer_boundry=la->first;
         auto  upper_boundry=la->second;
            if (lowwer_boundry!=upper_boundry) {
                 for (auto& its=lowwer_boundry;its!=upper_boundry;++its) {
                            tem.emplace_back(its.get_key(),its.get_value(),idx,0,its.get_tranc_id());
                 }
              }
      };
      if (la.has_value()){
             lam();
      }
      for (auto& it : frozen_tables) {
               la=it->iters_monotony_predicate(predicate);
               idx++;
                if (la.has_value()) {
                   lam();
              }
      }//HeapIterator的范围在迭代器的堆中，begin为current，end为堆空，即HeapIterator{}
      if (!tem.empty()) {
         return  std::make_pair(HeapIterator(tem, tranc_id), HeapIterator{});
      }
      return  std::nullopt;
 }
} // namespace tiny_lsm
