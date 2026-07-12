#include "lsm/engine.h"
#include "block/block_cache.h"
#include "config/config.h"
#include "consts.h"
#include "iterator/iterator.h"
#include "logger/logger.h"
#include "lsm/level_iterator.h"
#include "lsm/two_merge_iterator.h"
#include "memtable/memtable.h"
#include "spdlog/spdlog.h"
#include "sst/concact_iterator.h"
#include "sst/sst.h"
#include "sst/sst_iterator.h"
#include "vlog/vlog.h"
#include "wal/record.h"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <linux/falloc.h>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tiny_lsm {

// *********************** LSMEngine ***********************
LSMEngine::LSMEngine(std::string path) : data_dir(path) {
  // TODO: Lab 4.2 引擎初始化
  // ? 1. 初始化日志: init_spdlog_file()
  // ? 2. 初始化 block_cache (容量和 K 值从 TomlConfig 读取)
  // ? 3. 若目录不存在则创建
  // ? 4. 初始化 VLog: vlog_ = VLog::open(data_dir + "/vlog.data")
  // ? 5. 遍历目录加载所有已存在的 SST 文件:
  // ?    - 文件名格式: sst_{id}.{level}
  // ?    - 调用 SST::open 并记录到 ssts 和 level_sst_ids
  // ?    - 维护 next_sst_id 和 cur_max_level
  // ? 6. next_sst_id 自增
  // ? 7. 对各层 sst_id_list 排序; L0 层需要 reverse (越大的 id 越新, 优先查询)
  init_spdlog_file();
  block_cache = std::make_shared<BlockCache>(
      TomlConfig::getInstance().getLsmBlockCacheCapacity(),
      TomlConfig::getInstance().getLsmBlockCacheK());

  // 判断是否存在目录
  bool is_exit_dir = true;
  // 文件管理 filesystem
  if (!std::filesystem::is_directory(data_dir)) {
    std::filesystem::create_directory(data_dir);
    is_exit_dir = false;
  }
  // vlog
  if (TomlConfig::getInstance().getWisckeyValueThreshold() > 0) {
    vlog_ = VLog::open(data_dir + "/vlog.data");
    storage_mode_ = 1;
  }
  // 启动flush线程
  start_flush_thread();
  //启动comoact
  start_compact_thread();
  // 判断是否返回
  if (!is_exit_dir) {
    return;
  }
  // 文件创建打开 ofstream
  size_t max_sst_id = 0;
  for (const auto &entry : std::filesystem::directory_iterator(data_dir)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    std::string filename = entry.path().filename().string();
    if (filename.substr(0, 4) != "sst_") {
      continue;
    }
    int first_pos = filename.find_last_of(".");
    size_t level_ = std::stoi(
        filename.substr(first_pos + 1, filename.size() - first_pos - 1));
    size_t sst_id_ = std::stoi(filename.substr(4, first_pos - 4));
    //next_sst_id = std::max(sst_id_, (size_t)next_sst_id);
    max_sst_id = std::max(max_sst_id, sst_id_);
    cur_max_level = std::max(cur_max_level, level_);
    auto sst = SST::open(sst_id_, FileObj::open(entry.path().string(), false),
                         block_cache, vlog_);
    ssts.emplace(sst_id_, sst);
    level_sst_ids[level_].emplace_back(sst_id_);
    // auto &level_queue = level_sst_ids[level_];
    // auto pos =
    //     std::lower_bound(level_queue.begin(), level_queue.end(), sst_id_);
    // level_queue.insert(pos, sst_id_);
  }
  next_sst_id.store(max_sst_id + 1);
  
  // std::reverse(level_sst_ids[0].begin(), level_sst_ids[0].end());
  for (auto &[level, ids] : level_sst_ids) {
    if (level == 0) {
      std::sort(ids.begin(), ids.end(), std::greater<size_t>());
    } else {
      // 默认是<,应该只用ssd——id就可以
      //        std::sort(ids.begin(), ids.end(), [this](size_t& a, size_t& b) {
      //            return ssts[a]->get_first_key() < ssts[b]->get_first_key();
      // });
      std::sort(ids.begin(), ids.end());
    }
  }
}

LSMEngine::~LSMEngine() {
  stop_compact_thread();
  stop_flush_thread();
}

std::optional<std::pair<std::string, uint64_t>>
LSMEngine::get(const std::string &key, uint64_t tranc_id) {
  // TODO: Lab 4.2 查询
  // ? 1. 先查 memtable.get(key, tranc_id), 命中则返回 (value 非空) 或 nullopt
  // (value 为空=删除) ? 2. 加 ssts_mtx 读锁, 遍历 L0 的 sst_ids (越大越新),
  // 通过 sst->get() 查询 ? 3. 遍历 L1 及以上各层, 对每层做二分查找确定 key
  // 所在的 SST 文件 ? 注意: value 为空字符串表示 key 已被删除, 此时返回 nullopt
  auto mem_result = memtable.get(key, tranc_id);
  if (mem_result.is_valid()) {
    auto value = mem_result.get_value();
    // 考虑随便抽一个sst，还是加入一个专门用于干这个的sst
    // 可能要将resolve改到这里，但是很麻烦
    // auto value=resolve_value_try(mem_result.get_value());
    if (value.empty()) {
      return std::nullopt;
    } else {
      // return std::make_pair(value, mem_result.get_tranc_id());
      return std::make_pair(resolve_value_try(mem_result.get_value()),
                            mem_result.get_tranc_id());
    }
  }
  std::shared_lock<std::shared_mutex> lock(ssts_mtx);
  // 测试错误
  //   if (key == "key1000001") {
  //   std::cout << "debug get " << key << ", tranc_id=" << tranc_id <<
  //   std::endl; for (auto& [level, ids] : level_sst_ids) {
  //     std::cout << "level " << level << " size=" << ids.size() << std::endl;
  //     for (auto id : ids) {
  //       auto sst = ssts[id];
  //       bool cover = sst->get_first_key() <= key && key <=
  //       sst->get_last_key(); std::cout << "  sst " << id
  //                 << " [" << sst->get_first_key()
  //                 << ", " << sst->get_last_key()
  //                 << "] cover=" << cover << std::endl;
  //     }
  //   }
  // }
  for (auto &[level, tmp] : level_sst_ids) {
    // auto &tmp = level_sst_ids[i];
    if (level == 0) {
      for (auto sst_id : tmp) {
        auto sst_it = ssts[sst_id]->get(key, tranc_id);
        if (sst_it.is_valid()) {
          auto value = sst_it.value();
          if (value.empty()) {
            return std::nullopt;
          } else {
            // return std::make_pair(value, sst_it.get_cur_tranc_id());
            return std::make_pair(resolve_value_try(value),
                                  sst_it.get_cur_tranc_id());
          }
        }
      }
    } else {
      int left = 0, right = tmp.size() - 1, mid = -1;
      while (left <= right) {
        mid = left + (right - left) / 2;
        auto sst = ssts[tmp[mid]];
        if (key > sst->get_last_key()) {
          left = mid + 1;
        } else if (key < sst->get_first_key()) {
          right = mid - 1;
        } else {
          // key is in this SST
          auto sst_it = sst->get(key, tranc_id);

          if (sst_it.is_valid()) {
            auto value = sst_it.value();

            if (value.empty()) {
              return std::nullopt;
            } else {
              // return std::make_pair(value, sst_it.get_cur_tranc_id());
              return std::make_pair(resolve_value_try(value),
                                    sst_it.get_cur_tranc_id());
            }
          }
          break;
        }
      }
    }
  }
  return std::nullopt;
}

std::vector<
    std::pair<std::string, std::optional<std::pair<std::string, uint64_t>>>>
LSMEngine::get_batch(const std::vector<std::string> &keys, uint64_t tranc_id) {
  // TODO: Lab 4.2 批量查询
  // ? 1. 先从 memtable 批量查询: memtable.get_batch(keys, tranc_id)
  // ? 2. 若有未命中项, 加读锁后依次查 L0 各 SST 文件
  // ? 3. 若仍有未命中, 对各高层 SST 做二分查找补全结果
  // 找到与未找到
  auto results = memtable.get_batch(keys, tranc_id);
  //  std::all_of(...)   所有都满足
  // std::any_of(...)    至少一个满足
  // std::none_of(...)   一个都不满足
  // 由于无法遍历全部，放弃
  // auto is_not_find_all =
  //     std::any_of(results.begin(), results.end(), [this]( auto &item) {
  //       if (item.second.has_value()) {
  //          auto& tmp=item.second.value();
  //          tmp.first=resolve_value_try(tmp.first);
  //         return false;
  //       }
  //       return true;
  //     });
  // if (!is_not_find_all) {
  //   return results;
  // }
  bool has_missing = false;

  for (auto &item : results) {
    if (item.second.has_value()) {
      item.second->first = resolve_value_try(item.second->first);
    } else {
      has_missing = true;
    }
  }

  if (!has_missing) {
    return results;
  }
  // 不调用get，会在metable中重复
  std::shared_lock<std::shared_mutex> lock(ssts_mtx);
  for (int i = 0; i < results.size(); ++i) {
    if (results[i].second.has_value()) {
      continue;
    }
    auto key = results[i].first;
    for (auto &[level, tmp] : level_sst_ids) {
      // auto &tmp = level_sst_ids[i];
      bool is_find = false;
      if (level == 0) {
        for (auto &it : tmp) {
          auto sst_it = ssts[it]->get(key, tranc_id);
          if (sst_it.is_valid()) {
            if (!sst_it.value().empty()) {
              // results[i].second =
              //     std::make_pair(sst_it.value(), sst_it.get_cur_tranc_id());
              results[i].second = std::make_pair(
                  resolve_value_try(sst_it.value()), sst_it.get_cur_tranc_id());
            }
            is_find = true;
            break;
          }
        }
      } else {
        int left = 0, right = tmp.size() - 1, mid = -1;
        while (left <= right) {
          mid = left + (right - left) / 2;
          auto sst = ssts[tmp[mid]];
          if (key > sst->get_last_key()) {
            left = mid + 1;
          } else if (key < sst->get_first_key()) {
            right = mid - 1;
          } else {
            // key is in this SST
            auto sst_it = sst->get(key, tranc_id);
            if (sst_it.is_valid()) {
              if (!sst_it.value().empty()) {
                // results[i].second =
                //     std::make_pair(sst_it.value(),
                //     sst_it.get_cur_tranc_id());
                results[i].second =
                    std::make_pair(resolve_value_try(sst_it.value()),
                                   sst_it.get_cur_tranc_id());
              }
              is_find = true;
              break;
            }
          }
        }
      }
      if (is_find) {
        break;
      }
    }
  }
  return results;
}
// ？为什么不在get中用sst_get_
std::optional<std::pair<std::string, uint64_t>>
LSMEngine::sst_get_(const std::string &key, uint64_t tranc_id) {
  // TODO: Lab 4.2 sst 内部查询 (不查 memtable)
  // ? 逻辑与 get() 的 SST 部分相同, 先 L0 后 L1+
  for (auto &[level, tmp] : level_sst_ids) {
    // auto& tmp=level_sst_ids[i];
    if (level == 0) {
      for (auto &it : tmp) {
        auto sst_it = ssts[it]->get(key, tranc_id);
        if (sst_it.is_valid()) {
          if (!sst_it.value().empty()) {
            // return  std::make_pair(sst_it.value(),sst_it.get_cur_tranc_id());
            return std::make_pair(resolve_value_try(sst_it.value()),
                                  sst_it.get_cur_tranc_id());
          } else {
            return std::nullopt;
          }
        }
      }
    } else {
      int left = 0, right = tmp.size() - 1, mid = -1;
      while (left <= right) {
        mid = left + (right - left) / 2;
        auto sst = ssts[tmp[mid]];
        if (key < sst->get_first_key()) {
          right = mid - 1;
        } else if (key > sst->get_last_key()) {
          left = mid + 1;
        } else {
          auto sst_it = sst->get(key, tranc_id);
          if (sst_it.is_valid()) {
            if (!sst_it.value().empty()) {
              // return
              // std::make_pair(sst_it.value(),sst_it.get_cur_tranc_id());
              return std::make_pair(resolve_value_try(sst_it.value()),
                                    sst_it.get_cur_tranc_id());
            } else {
              return std::nullopt;
            }
          }
          break;
        }
      }
    }
  }
  return std::nullopt;
}
// 将resolve移动到这里，sst，memtable的返回值需要解码，sst的resolve被注释
std::string LSMEngine::resolve_value_try(const std::string &raw_value) const {
  // WiscKey 模式下: raw_value 是 12 字节的 vlog 引用 [offset:8][size:4]
  // 普通模式下直接返回 raw_value
  if (storage_mode_ == 0 || raw_value.empty()) {
    return raw_value;
  }
  // 超过100万会出错
  // 原来的实验无法区分12的数据和引用
  // 等于时也转为了引用
  if (raw_value.size() < sizeof(uint64_t) + sizeof(uint32_t)) {
    return raw_value;
  }
  uint64_t off = 0;
  uint32_t sz = 0;
  memcpy(&off, raw_value.data(), sizeof(uint64_t));
  memcpy(&sz, raw_value.data() + sizeof(uint64_t), sizeof(uint32_t));
  if (!vlog_) {
    throw std::runtime_error(
        "SST::resolve_value: vlog is null for WiscKey SST");
  }
  return vlog_->read_value(off, sz);
}
// 对vlog的转换
std::string LSMEngine::tran_vlog(const std::string &key,
                                 const std::string &value) {
  auto wisckey_value_threshold_ =
      TomlConfig::getInstance().getWisckeyValueThreshold();
  // 大于12后，在12和限制之间的处理丢失
  if (wisckey_value_threshold_ > sizeof(uint64_t) + sizeof(uint32_t)) {
    throw std::runtime_error("WiscKey threshold must be <= 12");
  }
  if (vlog_ && wisckey_value_threshold_ > 0 &&
      value.size() >= wisckey_value_threshold_) {
    std::string vl_(sizeof(uint64_t) + sizeof(uint32_t), '\0');
    auto offset = vlog_->append(key, value);
    memcpy(vl_.data(), &offset, sizeof(uint64_t));
    auto va_size = static_cast<uint32_t>(value.size());
    memcpy(vl_.data() + sizeof(uint64_t), &va_size, sizeof(uint32_t));
    return vl_;
  }
  return value;
}
//vlog的批量写入
std::vector<std::pair<std::string, std::string>>
LSMEngine::tran_vlog_batch(
    const std::vector<std::pair<std::string, std::string>> &kvs) {
  auto threshold = TomlConfig::getInstance().getWisckeyValueThreshold();
  if (threshold > sizeof(uint64_t) + sizeof(uint32_t)) {
    throw std::runtime_error("WiscKey threshold must be <= 12");
  }

  std::vector<std::pair<std::string, std::string>> encoded = kvs;
  if (!vlog_ || threshold == 0) {
    return encoded;
  }

  std::vector<std::pair<std::string, std::string>> large;
  std::vector<size_t> idxs;

  for (size_t i = 0; i < kvs.size(); ++i) {
    if (kvs[i].second.size() >= threshold) {
      idxs.emplace_back(i);
      large.emplace_back(kvs[i]);
    }
  }

  auto offsets = vlog_->append_batch(large);

  for (size_t i = 0; i < offsets.size(); ++i) {
    std::string ref(sizeof(uint64_t) + sizeof(uint32_t), '\0');
    uint64_t offset = offsets[i];
    uint32_t size = static_cast<uint32_t>(large[i].second.size());
    memcpy(ref.data(), &offset, sizeof(uint64_t));
    memcpy(ref.data() + sizeof(uint64_t), &size, sizeof(uint32_t));
    encoded[idxs[i]].second = std::move(ref);
  }

  return encoded;
}
// 加的检查写冲突的函数
bool LSMEngine::chech_write(const std::string &key, const uint64_t &tranc_id_) {
  auto get_mem = memtable.get_(key, 0);
  // memtable中找到且冲突
  if (get_mem.is_valid() && get_mem.get_tranc_id() > tranc_id_) {

    auto key_ = key;
    // abort();
    spdlog::warn("TranContext--commit(): Conflict detected on key={}, "
                 "aborting transaction ID={}",
                 key, tranc_id_);
    return true;
  }
  // 没找到，在sst中找
  if (get_mem.is_end() &&
      tran_manager.lock()->get_max_flushed_tranc_id() > tranc_id_) {
    //  if (tran_manager.lock()->get_max_flushed_tranc_id() <= tranc_id_) {
    // sst 中最大的 tranc_id 小于当前 tranc_id, 没有冲突
    //           continue;
    //       }
    auto get_sst = sst_get_(key, 0);
    if (get_sst && get_sst.value().second > tranc_id_) {
      auto key_ = key;
      spdlog::warn("TranContext--commit(): Conflict detected on key={}, "
                   "aborting transaction ID={}",
                   key, tranc_id_);
      return true;
    }
  }
  return false;
}
uint64_t LSMEngine::put(const std::string &key, const std::string &value,
                        uint64_t tranc_id) {
  // TODO: Lab 4.1 插入
  // ? 调用 memtable.put(key, value, tranc_id)
  // ? 若 memtable 总大小 >= LsmTolMemSizeLimit 则调用 flush() 并返回其结果
  // ? 否则返回 0

  memtable.put(key, value, tranc_id);
  bool need_flush = false;
  if (memtable.current_table->get_size() >=
      TomlConfig::getInstance().getLsmPerMemSizeLimit()) {
    memtable.frozen_cur_table_();
    need_flush = true;
  }
  if (need_flush) {
    request_flush();
  }
  return 0;
}

uint64_t LSMEngine::put_batch(
    const std::vector<std::pair<std::string, std::string>> &kvs,
    uint64_t tranc_id) {
  // TODO: Lab 4.1 批量插入
  // ? 调用 memtable.put_batch(kvs, tranc_id)
  // ? 若超限则 flush() 并返回其结果
  memtable.put_batch(kvs, tranc_id);
  bool need_flush = false;
  if (memtable.current_table->get_size() >=
      TomlConfig::getInstance().getLsmPerMemSizeLimit()) {
    memtable.frozen_cur_table_();
    need_flush = true;
  }
  if (need_flush) {
    request_flush();
  }
  return 0;
}

uint64_t LSMEngine::remove(const std::string &key, uint64_t tranc_id) {
  // TODO: Lab 4.1 删除
  // ? 在 LSM 中，删除实际上是插入一个空值
  // ? 调用 memtable.remove(key, tranc_id)
  // ? 若超限则 flush() 并返回其结果
  memtable.remove(key, tranc_id);
  bool need_flush = false;
  if (memtable.current_table->get_size() >=
      TomlConfig::getInstance().getLsmPerMemSizeLimit()) {
    memtable.frozen_cur_table_();
    need_flush = true;
  }
  if (need_flush) {
    request_flush();
  }
  return 0;
}

uint64_t LSMEngine::remove_batch(const std::vector<std::string> &keys,
                                 uint64_t tranc_id) {
  // TODO: Lab 4.1 批量删除
  // ? 调用 memtable.remove_batch(keys, tranc_id)
  // ? 若超限则 flush() 并返回其结果
  memtable.remove_batch(keys, tranc_id);
  bool need_flush = false;
  if (memtable.current_table->get_size() >=
      TomlConfig::getInstance().getLsmPerMemSizeLimit()) {
    memtable.frozen_cur_table_();
    need_flush = true;
  }
  if (need_flush) {
    request_flush();
  }
  return 0;
}

void LSMEngine::clear() {
 //stop_flush_thread() ;
  memtable.clear();
  level_sst_ids.clear();
  ssts.clear();
  // 清空当前文件夹的所有内容
  try {
    for (const auto &entry : std::filesystem::directory_iterator(data_dir)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      std::filesystem::remove(entry.path());

      spdlog::info("LSMEngine--"
                   "clear file {} successfully.",
                   entry.path().string());
    }
  } catch (const std::filesystem::filesystem_error &e) {
    // 处理文件系统错误
    spdlog::error("Error clearing directory: {}", e.what());
  }

  // Re-create the vlog so new writes go to a fresh file
  if (vlog_) {
    vlog_->del_vlog();
    vlog_ = VLog::open(data_dir + "/vlog.data");
  }
}
// flush异步
void LSMEngine::start_flush_thread() {
  flush_thread_ = std::thread(&LSMEngine::flush_worker, this);
}

void LSMEngine::stop_flush_thread() {
  flush_stop_ = true;
  flush_cv_.notify_all();
  if (flush_thread_.joinable()) {
    flush_thread_.join();
  }
}
void LSMEngine::request_flush() {
  {
    std::lock_guard<std::mutex> lock(flush_mtx_);
    flush_requested_ = true;
  }
  flush_cv_.notify_one();
}

void LSMEngine::flush_worker() {
  while (!flush_stop_) {
    {
      std::unique_lock<std::mutex> lock(flush_mtx_);
      flush_cv_.wait(lock, [&] { return flush_stop_ || flush_requested_; });
      flush_requested_ = false;
    }
   
       flush_one_frozens();
  }
}
void LSMEngine::flush_one_frozens() {
  std::unique_lock<std::mutex> job_lock(flush_job_mtx_);
  // ratio每层的放大比例，level一般是4，实验中把比例也设置为4
  // if (level_sst_ids[0].size() >=
  //     TomlConfig::getInstance().getLsmSstLevelRatio()) {
  //    request_compact(0);
  // }
  if (memtable.get_frozen_size() == 0) {
      return;
}
  size_t sst_id = next_sst_id.fetch_add(1);
  
  auto wk = TomlConfig::getInstance().getWisckeyValueThreshold();
  // SSTBuilder builder(TomlConfig::getInstance().getLsmBlockSize(),
  // true,vlog_,TomlConfig::getInstance().getWisckeyValueThreshold());
  std::unique_ptr<SSTBuilder> build;
  if (vlog_ && wk > 0) {
    build = std::make_unique<SSTBuilder>(
        TomlConfig::getInstance().getLsmBlockSize(), true, vlog_, wk);
  } else {
    build = std::make_unique<SSTBuilder>(
        TomlConfig::getInstance().getLsmBlockSize(), true);
  }
  std::vector<uint64_t> flushed_tranc_ids;
  // 在txn manager中记录已经flush的事务id，通知事务管理器这些事务已经完成
  // 也就是说空 key + 空 value 是“事务结束/提交标记”。
  //sst的构造在实际的结构中并不需要锁，锁更多是保证逻辑
  std::shared_ptr<SkipList> flushed_table;
  std::string sst_path = get_sst_path(sst_id, 0);
  //flush_last() 会先把 frozen table 从 frozen_tables 移走，再 build SST。这个窗口里，
  // 读请求可能既查不到 frozen table，也查不到还没发布的 SST
  //需要加 flushing_tables，读 memtable 时也查它
  //已经解决，flushed_table在完成sst后再删除，通过memtable.remove_flushed_table_(flushed_table)

  //fluhsh也过滤mvcc，防止太多同key被写入同一个block
  uint64_t watermark = tran_manager.lock()
                         ? tran_manager.lock()->get_oldest_active_tranc_id()
                         : 0;
  auto sst_ptr = memtable.flush_last(*build, sst_path, sst_id,
                                     flushed_tranc_ids, block_cache,flushed_table,watermark);
 
  if (!sst_ptr) {
    return ;
  }
   bool need_compact = false;
  {
    std::unique_lock<std::shared_mutex> lock(ssts_mtx);

    ssts.emplace(sst_id, sst_ptr);
    level_sst_ids[0].emplace_front(sst_id);

    need_compact = level_sst_ids[0].size() >=
        TomlConfig::getInstance().getLsmSstLevelRatio();
  }
  memtable.remove_flushed_table_(flushed_table);
  // for (auto &id : flushed_tranc_ids) {
  //   tran_manager.lock()->add_flushed_tranc_id(id);
  // }
if (auto tm = tran_manager.lock()) {
  tm->add_flushed_tranc_ids(flushed_tranc_ids);
}
  if (need_compact) {
    request_compact(0);
  }
  return;
}

std::string LSMEngine::get_sst_path(size_t sst_id, size_t target_level) {
  // sst的文件路径格式为: data_dir/sst_<sst_id>，sst_id格式化为32位数字
  std::stringstream ss;
  ss << data_dir << "/sst_" << std::setfill('0') << std::setw(32) << sst_id
     << '.' << target_level;
  return ss.str();
}

//compact异步
void LSMEngine::start_compact_thread() {
  compact_thread_ = std::thread(&LSMEngine::compact_worker, this);
}

void LSMEngine::stop_compact_thread() {
  compact_stop_ = true;
  compact_cv_.notify_all();
  if (compact_thread_.joinable()) {
    compact_thread_.join();
  }
}

void LSMEngine::request_compact(size_t level) {
  {
    std::lock_guard<std::mutex> lock(compact_mtx_);
    if (compact_requested_) {
      return;
    }
     compact_requested_ = true;
  }
  compact_cv_.notify_one();
}

void LSMEngine::compact_worker() {
  while (!compact_stop_) {
    //size_t level = 0;

    {
      std::unique_lock<std::mutex> lock(compact_mtx_);
      compact_cv_.wait(lock, [&] {
        return compact_stop_ || compact_requested_ ;
;
      });

      if (compact_stop_) {
        return;
      }

      compact_requested_ = false;
    }

    //std::unique_lock<std::shared_mutex> lock(ssts_mtx);
    full_compact(0);
  }
}
std::optional<std::pair<TwoMergeIterator, TwoMergeIterator>>
LSMEngine::lsm_iters_monotony_predicate(
    uint64_t tranc_id, std::function<int(const std::string &)> predicate) {
  // TODO: Lab 4.7 谓词查询
  // ? 1. 从 memtable 查询: memtable.iters_monotony_predicate(tranc_id,
  // predicate) ? 2. 遍历所有 SST, 对每个 SST 调用 sst_iters_monotony_predicate
  // ?    将所有结果合并到 item_vec (注意过滤事务可见性和相同 key
  // 只保留最新版本) ? 3. 构造 TwoMergeIterator 合并 memtable 结果和 sst 结果
  // ? 4. 若均为空返回 nullopt
  std::shared_lock<std::shared_mutex> lock(ssts_mtx);
  // 1. 从 memtable 查询
  if (memtable.get_total_size() == 0 && ssts.empty()) {
    return std::nullopt;
  }
  std::shared_ptr<HeapIterator> mem_iter = nullptr;
  if (memtable.get_total_size() > 0) {
    auto mem_result = memtable.iters_monotony_predicate(tranc_id, predicate);
    if (mem_result) {
      // resolve
      std::vector<SearchItem> mem_items;

      auto mem_begin = mem_result->first;
      auto mem_end = mem_result->second;

      for (auto it = mem_begin; it != mem_end; ++it) {
        auto raw = *it;
        if (!raw.second.empty()) {
          mem_items.emplace_back(raw.first, resolve_value_try(raw.second), 0, 0,
                                 it.get_tranc_id());
        }
      }
      mem_iter = std::make_shared<HeapIterator>(std::move(mem_items), tranc_id);
    }
  }
  // sst的查询与合并
  std::vector<SearchItem> tem;
  if (ssts.size() > 0) {
    for (auto &it : level_sst_ids) {
      for (auto &sst_id : it.second) {
        // 范围查询，没有用到bloom
        auto sst =
            sst_iters_monotony_predicate(ssts[sst_id], tranc_id, predicate);
        if (sst) {
          auto lowwer_boundry = sst->first;
          auto upper_boundry = sst->second;
          if (lowwer_boundry != upper_boundry) {
            for (auto &its = lowwer_boundry; its != upper_boundry&& its.is_valid(); ++its) {
              // DEBUG MonotonyPredicate: remove after checking SST collection.
              // std::cout << "[sst_collect] key=" << its.key()
              //           << " value=" << its.value()
              //           << " txn=" << its.get_cur_tranc_id()
              //           << "\n";
              // if (!its.value().empty()) {
              //   // tem.emplace_back(its.key(),its.value(),sst_id,it.first,its.get_cur_tranc_id());
              //   tem.emplace_back(its.key(), resolve_value_try(its.value()),
              //                    sst_id, it.first, its.get_cur_tranc_id());
              auto raw = *its;
              if (!raw.second.empty()) {
                tem.emplace_back(raw.first, resolve_value_try(raw.second),
                                 sst_id, it.first, its.get_cur_tranc_id());
              }
            }
          }
        }
      }
    }
  }
  if (tem.empty() && !mem_iter) {
    return std::nullopt;
  }
  // heap的构造，只有
  return std::make_pair(
      TwoMergeIterator(mem_iter,
                       std::make_shared<HeapIterator>(std::move(tem), tranc_id),
                       tranc_id),
      TwoMergeIterator(nullptr, nullptr, tranc_id));
}

Level_Iterator LSMEngine::begin(uint64_t tranc_id) {
  // TODO: Lab 4.7
  // ? 返回 Level_Iterator(shared_from_this(), tranc_id)
  return Level_Iterator(shared_from_this(), tranc_id);
}

Level_Iterator LSMEngine::end() {
  // TODO: Lab 4.7
  // ? 返回空的 Level_Iterator{}
  return Level_Iterator{};
}

// void LSMEngine::full_compact(size_t src_level) {
//   // TODO: Lab 4.5 负责完成整个 full compact
//   // ? 1. 递归判断下一级 level 是否需要 compact
//   // (level_sst_ids[src_level+1].size() >= ratio) ? 2. 根据 src_level 是否为 0
//   // 分别调用 full_l0_l1_compact 或 full_common_compact ? 3. 删除旧 SST 文件并从
//   // ssts/level_sst_ids 中移除记录 ? 4. 将新的 SST 加入
//   // level_sst_ids[src_level+1] 并排序 ? 5. 更新 cur_max_level

//   // 每层的数量ratio为4，sst的size也为4
//   if (level_sst_ids[src_level].size() <
//       TomlConfig::getInstance().getLsmSstLevelRatio() * (src_level + 1)) {
//     return;
//   }
//   {
//     // 合并时不应该运行读写
//     // std::unique_lock<std::shared_mutex> lock(ssts_mtx);，flush已经有了
//     auto vec_ids = [this](size_t level, std::vector<size_t> &ans) {
//       for (auto &it : level_sst_ids[level]) {
//         ans.emplace_back(it);
//       }
//     };
//     std::vector<size_t> lx;
//     std::vector<size_t> ly;
//     vec_ids(src_level, lx);
//     vec_ids(src_level + 1, ly);
//     std::vector<std::shared_ptr<SST>> ans;
//     if (src_level == 0) {
//       ans = full_l0_l1_compact(lx, ly);
//     } else {
//       ans = full_common_compact(lx, ly, src_level + 1);
//     }
//     // 判断compact是否合理
//     if (ans.empty()) {
//       return;
//     }
//     // 删除旧 SST 文件并从 ssts/level_sst_ids 中移除记录
//     auto update_sst = [this](std::vector<size_t> &ids, size_t level) {
//       for (auto &it : ids) {
       
//         ssts.erase(it);
//       }
//       level_sst_ids[level].clear();
//     };
//     update_sst(lx, src_level);
//     update_sst(ly, src_level + 1);
//     // 将新的 SST 加入 level_sst_ids[src_level+1] 并排序
//     for (auto &it : ans) {
//       ssts.emplace(it->get_sst_id(), it);
//       level_sst_ids[src_level + 1].emplace_back(it->get_sst_id());
//       // sst文件的名字在fgen_sst_from_iter中已经生成了，文件的路径也在get_sst_path中生成了，所以这里不需要再生成一次了
//     }
//     // 更新 cur_max_level
//     cur_max_level = std::max(cur_max_level, src_level + 1);
//   }
//   // 递归下一层是否需要compact
//   full_compact(src_level + 1);
// }

//修改后的full，不在全程持有sst，在和并时并不需要，在level_sst_ids和ssts中组织才是level
void LSMEngine::full_compact(size_t src_level) {
  const auto ratio = TomlConfig::getInstance().getLsmSstLevelRatio();
  std::vector<size_t> lx_ids;
  std::vector<size_t> ly_ids;
  std::vector<std::shared_ptr<SST>> lx_ssts;
  std::vector<std::shared_ptr<SST>> ly_ssts;
  {
    std::shared_lock<std::shared_mutex> lock(ssts_mtx);
    if (level_sst_ids[src_level].size() < ratio * (src_level + 1)) {
            return;
  }
    //sst的id
    lx_ids.assign(level_sst_ids[src_level].begin(),
                  level_sst_ids[src_level].end());
    ly_ids.assign(level_sst_ids[src_level + 1].begin(),
                  level_sst_ids[src_level + 1].end());
    
   auto vec_sst = [this](size_t level, std::vector<size_t> &ix_ids,
    std::vector<std::shared_ptr<SST>> &lx_ssts) {
      for (auto id : ix_ids) {
      auto it = ssts.find(id);
      if (it != ssts.end()) {
        lx_ssts.emplace_back(it->second);
      }
    }
    };
   vec_sst(src_level,lx_ids,lx_ssts);
    vec_sst(src_level + 1, ly_ids,ly_ssts);
  }
  //防止出错
  if (lx_ssts.size() != lx_ids.size() || ly_ssts.size() != ly_ids.size()) {
    return;
  }
  std::vector<std::shared_ptr<SST>> new_ssts;
  if (src_level == 0) {
    new_ssts = full_l0_l1_compact(lx_ssts, ly_ssts);
  } else {
    new_ssts = full_common_compact(lx_ssts, ly_ssts, src_level + 1);
  }

  if (new_ssts.empty()) {
    return;
  }
   bool need_next_compact = false;

  {
    std::unique_lock<std::shared_mutex> lock(ssts_mtx);
    //flush不断刷入，因此不能clear
    //remove把不等于id的都移动到前方，返回第一个id的位置
    //常见的 erase-remove idiom。
    // auto erase_id = [](auto &ids, size_t id) {
    //   ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
    // };
    auto erase_id = [](auto &ids, size_t id) {
      auto it = std::find(ids.begin(), ids.end(), id);
      if (it != ids.end()) {
        ids.erase(it);
      }
    };
    auto del_sst = [this,
                    &erase_id](size_t level, std::vector<size_t> &ix_ids,
                               std::vector<std::shared_ptr<SST>> &lx_ssts) {
      for (auto id : ix_ids) {
        auto it = ssts.find(id);
        if (it != ssts.end()) {
          it->second->del_sst();
          ssts.erase(it);
        }
        erase_id(level_sst_ids[level], id);
      }
    };

    del_sst(src_level, lx_ids, lx_ssts);
    del_sst(src_level + 1, ly_ids, ly_ssts);

    for (auto &sst : new_ssts) {
      ssts.emplace(sst->get_sst_id(), sst);
      level_sst_ids[src_level + 1].emplace_back(sst->get_sst_id());
    }

    cur_max_level = std::max(cur_max_level, src_level + 1);

    need_next_compact =
        level_sst_ids[src_level + 1].size() >= ratio * (src_level + 2);
  }

  if (need_next_compact) {
    full_compact(src_level + 1);
  }
}
std::vector<std::shared_ptr<SST>>
LSMEngine::full_l0_l1_compact(
    const std::vector<std::shared_ptr<SST>> &l0_ssts,
    const std::vector<std::shared_ptr<SST>> &l1_ssts) {
  if (l0_ssts.size() < TomlConfig::getInstance().getLsmSstLevelRatio()) {
    return {};
  }

  std::vector<SstIterator> l0_iters;
  l0_iters.reserve(l0_ssts.size());

  for (auto &sst : l0_ssts) {
    l0_iters.emplace_back(sst, 0, true);
  }

  auto l0_heap = SstIterator::merge_sst_iterator(l0_iters, 0, true);

  TwoMergeIterator iter(
      std::make_shared<HeapIterator>(l0_heap.first),
      std::make_shared<ConcactIterator>(l1_ssts, 0, true),
      0,
      true);

  return gen_sst_from_iter(iter, get_sst_size(1), 1);
}
std::vector<std::shared_ptr<SST>>
LSMEngine::full_common_compact(
    const std::vector<std::shared_ptr<SST>> &lx_ssts,
    const std::vector<std::shared_ptr<SST>> &ly_ssts,
    size_t level_y) {
  if (lx_ssts.empty()) {
    return {};
  }

  TwoMergeIterator iter(
      std::make_shared<ConcactIterator>(lx_ssts, 0, true),
      std::make_shared<ConcactIterator>(ly_ssts, 0, true),
      0,
      true);

  return gen_sst_from_iter(iter, get_sst_size(level_y), level_y);
}
// std::vector<std::shared_ptr<SST>>
// LSMEngine::full_l0_l1_compact(std::vector<size_t> &l0_ids,
//                               std::vector<size_t> &l1_ids) {
  // TODO: Lab 4.5 负责完成 l0 和 l1 的 full compact
  // ? L0 各 SST 的 key 有重叠, 需要先通过 SstIterator::merge_sst_iterator 合并
  // ? 再用 TwoMergeIterator 与 L1 的 ConcactIterator 合并
  // ? 最后调用 gen_sst_from_iter 生成新的 SST 文件 (目标大小 = PerMemSizeLimit
  // * SstLevelRatio)
//   if (l0_ids.size() < TomlConfig::getInstance().getLsmSstLevelRatio()) {
//     return {};
//   }
//   std::vector<std::shared_ptr<SST>> l1_sst;
//   for (auto &it : l1_ids) {
//     l1_sst.emplace_back(ssts[it]);
//   }
//   std::vector<SstIterator> l0_sstT;
//   for (auto &it : l0_ids) {
//     l0_sstT.emplace_back(ssts[it], 0, true);
//   }
//   auto l0_heap = SstIterator::merge_sst_iterator(l0_sstT, 0, true);
//   TwoMergeIterator t_mer(std::make_shared<HeapIterator>(l0_heap.first),
//                          std::make_shared<ConcactIterator>(l1_sst, 0, true), 0,
//                          true);
//   auto ans = gen_sst_from_iter(t_mer, get_sst_size(1), 1);
//   return ans;
// }

// std::vector<std::shared_ptr<SST>>
// LSMEngine::full_common_compact(std::vector<size_t> &lx_ids,
//                                std::vector<size_t> &ly_ids, size_t level_y) {
  // TODO: Lab 4.5 负责完成其他相邻 level 的 full compact
  // ? Lx 和 Ly 都是有序不重叠的 SST, 直接用 ConcactIterator 遍历
  // ? 通过 TwoMergeIterator 合并后调用 gen_sst_from_iter
//   if (level_sst_ids[level_y - 1].size() <
//       TomlConfig::getInstance().getLsmSstLevelRatio() * (level_y - 1)) {
//     return {};
//   }
//   std::vector<std::shared_ptr<SST>> lx_ssts, ly_ssts;
   // const& 不能修改，也因此emplace报错
//   auto sst_ids = [this](std::vector<size_t> &ids,
//                         std::vector<std::shared_ptr<SST>> &sst_vec) {
//     for (auto &it : ids) {
//       sst_vec.emplace_back(ssts[it]);
//     }
//   };
//   sst_ids(lx_ids, lx_ssts);
//   sst_ids(ly_ids, ly_ssts);
//   TwoMergeIterator two_merge_iter(
//       std::make_shared<ConcactIterator>(lx_ssts, 0, true),
//       std::make_shared<ConcactIterator>(ly_ssts, 0, true), 0, true);
//   auto ans = gen_sst_from_iter(two_merge_iter, get_sst_size(level_y), level_y);
//   return ans;
// }

std::vector<std::shared_ptr<SST>>
LSMEngine::gen_sst_from_iter(BaseIterator &iter, size_t target_sst_size,
                             size_t target_level) {
  // TODO: Lab 4.5 实现从迭代器构造新的 SST
  // ? 循环从迭代器取 key-value 写入 SSTBuilder
  // ? 当 estimated_size >= target_sst_size 时 (注意不能在相同 key
  // 的不同版本之间切分) ?   调用 builder.build() 生成 SST 并重置 builder ?
  // 迭代结束后若 builder 非空则再次 build ? 注意: WiscKey 模式下需使用带 vlog
  // 参数的 SSTBuilder 构造函数

  // 在memtable中size'只计key+value，flush->l0时数据没错
  //  合并时size计的时block的key，value，offset，offset—size，那sst的数据达不到target_sst_size的纯数据
  if (iter.is_end()) {
    return {};
  }
  std::vector<std::shared_ptr<SST>> new_sst;
  std::string last_key; // SST 切分用
  std::pair<std::string, std::string> key;
  std::unique_ptr<SSTBuilder> build;
  uint64_t active_oldest_txn =
      tran_manager.lock()->get_oldest_active_tranc_id();
  bool is_keep_last = false;
  while (true) {
    size_t wk = TomlConfig::getInstance().getWisckeyValueThreshold();
    if (vlog_ && wk > 0) {
      build = std::make_unique<SSTBuilder>(
          TomlConfig::getInstance().getLsmBlockSize(), true, vlog_, wk);
    } else {
      build = std::make_unique<SSTBuilder>(
          TomlConfig::getInstance().getLsmBlockSize(), true);
    }
    while (iter.is_valid()) {
      // 当不同key时，刷新标记
      key = *iter;
      if (last_key != (*iter).first) {
        last_key = (*iter).first;
        is_keep_last = false;
      }
      bool is_add = false;
      if (iter.get_tranc_id() > active_oldest_txn) {
        is_add = true;
      }
      // 用is_keep_last作为标记，当第一次来到这里，并且<=，变为true
      if (!is_keep_last && iter.get_tranc_id() <= active_oldest_txn) {
        is_keep_last = true;
        is_add = true;
      }
      // 此时的txn_id,kepp_all开启即curren——txn，否则是最新即max_txn
      if (is_add) {
        build->add(key.first, key.second, iter.get_tranc_id());
      }
      ++iter;
      if (iter.is_end() || (last_key != (*iter).first &&
                            build->estimated_size() >= target_sst_size)) {
        break;
      }
    }
    if (build->real_size() > 0) {
      size_t sst_id = next_sst_id.fetch_add(1);
      auto path = get_sst_path(sst_id, target_level);
      new_sst.emplace_back(build->build(sst_id, path, block_cache));
    }
    if (iter.is_end()) {
      break;
    }
  }
  return new_sst;
}

size_t LSMEngine::get_sst_size(size_t level) {
  if (level == 0) {
    return TomlConfig::getInstance().getLsmPerMemSizeLimit();
  } else {
    return TomlConfig::getInstance().getLsmPerMemSizeLimit() *
           static_cast<size_t>(std::pow(
               TomlConfig::getInstance().getLsmSstLevelRatio(), level));
  }
}

void LSMEngine::set_tran_manager(std::shared_ptr<TranManager> tran_manager) {
  this->tran_manager = tran_manager;
}

// *********************** LSM ***********************
LSM::LSM(std::string path)
    : engine(std::make_shared<LSMEngine>(path)),
      tran_manager_(std::make_shared<TranManager>(path)) {
  // TODO: Lab 5.5 控制WAL重放与组件的初始化
  // ? 1. 绑定 tran_manager 与 engine: 互相 set
  // ? 2. 调用 tran_manager_->check_recover() 获取需要重放的事务记录
  // ? 3. 遍历返回的 map<tranc_id, records>:
  // ?    - 若该 tranc_id 已在 flushed_tranc_ids 中则跳过 (已刷盘无需重放)
  // ?    - 否则根据 record.getOperationType() 调用 engine->put() 或
  // engine->remove() ? 4. 调用 tran_manager_->init_new_wal() 开启新的 WAL
  // 文件准备接收新写入
  engine->set_tran_manager(tran_manager_);
  tran_manager_->set_engine(engine);
  auto recover_record = std::move(tran_manager_->check_recover());
  // 有问题，flushed_file,在析构在写入，而崩溃时析构不一定有效
  auto &flushed_txn = tran_manager_->get_flushed_tranc_ids();
  {
    std::unique_lock sst_lock(engine->ssts_mtx);
    std::unique_lock cur_lock(engine->memtable.cur_mtx);
    std::unique_lock frozen_lock(engine->memtable.frozen_mtx);
    // wal的txn也要重新put标志
    for (auto &it : recover_record) {
      if (flushed_txn.contains(it.first)) {
        continue; //  already flushed
      }
      for (auto &item : it.second) {
        if (item.getOperationType() == OperationType::OP_PUT) {
          engine->memtable.put_(item.getKey(), item.getValue(),
                                item.getTrancId());
        } else if (item.getOperationType() == OperationType::OP_DELETE) {
          engine->memtable.remove_(item.getKey(), item.getTrancId());
        }
      }
      engine->memtable.put_("", "", it.first);
      engine->memtable.maybe_frozen_cur_table_();
      tran_manager_->add_ready_to_flush_tranc_id(
          it.first, TransactionState::OP_COMMITTED);
    }
  }

  // 清理wal，开启新wal
  tran_manager_->init_new_wal();
}

LSM::~LSM() {
  engine->stop_flush_thread();
  engine->stop_compact_thread();
  flush_all();
  tran_manager_->write_tranc_id_file();
  //clear();
}

std::optional<std::string> LSM::get(const std::string &key) {
  auto tranc_id = tran_manager_->getNextTransactionId();
  auto res = engine->get(key, tranc_id);
  // auto res =engine->resolve_value_try(engine->get(key, tranc_id)->first);
  if (res.has_value()) {
    // return engine->resolve_value_try(res.value().first);
    return res.value().first;
  }
  return std::nullopt;
}

std::vector<std::pair<std::string, std::optional<std::string>>>
LSM::get_batch(const std::vector<std::string> &keys) {
  // 1. 获取事务ID
  auto tranc_id = tran_manager_->getNextTransactionId();

  // 2. 调用 engine 的批量查询接口
  auto batch_results = engine->get_batch(keys, tranc_id);

  // 3. 构造最终结果
  std::vector<std::pair<std::string, std::optional<std::string>>> results;
  for (const auto &[key, value] : batch_results) {
    if (value.has_value()) {
      // results.emplace_back(key, engine->resolve_value_try(value->first));
      results.emplace_back(key, value->first); // 提取值部分
    } else {
      results.emplace_back(key, std::nullopt); // 键不存在
    }
  }

  return results;
}
void LSM::put(const std::string &key, const std::string &value) {
  auto tranc_id = tran_manager_->getNextTransactionId();
  
    // vlog
    const auto &threshold =
        TomlConfig::getInstance().getWisckeyValueThreshold();
    bool use_vlog = engine->vlog_ && threshold > 0 && value.size() >= threshold;

    std::string value_ = use_vlog ? engine->tran_vlog(key, value) : value;
    std::vector<Record> operations;
    //   if (engine->vlog_ && threshold > 0 && value.size() >= threshold) {
    //    engine->vlog_->sync();
    //  }
    // operations.emplace_back(Record::createRecord(tranc_id));
    operations.emplace_back(Record::putRecord(tranc_id, key, value_));
    operations.emplace_back(Record::commitRecord(tranc_id));

    if (!tran_manager_->write_to_wal(operations)) {
      spdlog::error(
          "TranContext--commit(): Failed to write WAL for transaction ID={}",
          tranc_id);
      throw std::runtime_error("write to wal failed");
    }
    // spdlog::info(
    //     "TranContext--commit(): Transaction ID={} committed successfully",
    //     tranc_id);
    // spdlog::trace(
    //     "TranContext--commit(): Transaction ID={} committed successfully",
    //     tranc_id);
    bool need_flush = false;
    {
    std::unique_lock skip_lock(engine->memtable.cur_mtx);
    std::unique_lock frozen_lock(engine->memtable.frozen_mtx);
    // 写入memtable
    engine->memtable.put_(key, value_, tranc_id);
    // 检测flushed
    engine->memtable.put_("", "", tranc_id);
    //  增加skiplist
    //engine->memtable.maybe_frozen_cur_table_();
      if (engine->memtable.current_table->get_size() >= TomlConfig::getInstance().getLsmPerMemSizeLimit()) {
      //std::unique_lock lock(frozen_mtx);
       engine->memtable.frozen_cur_table_();
            need_flush = true;
    }
  }
  tran_manager_->add_ready_to_flush_tranc_id(tranc_id,
                                             TransactionState::OP_COMMITTED);
  if (need_flush) {
  engine->request_flush();
}
}
// 实验没有处理单条事务（自动提交）的wal和vlog
// 在txn-manger中，使用的是engine，因此自动的走这个
// 在这里进行更改
// 在commit和这里用的put——，没有判断单个的skiplist，因此不会自增，但是没事
// flush的标准是总的字节，后续在compact中会变为标准的大小
// 写冲突检测版本
//  void LSM::put(const std::string &key, const std::string &value) {
//    auto tranc_id = tran_manager_->getNextTransactionId();

//   {
//      std::unique_lock sst_lock(engine->ssts_mtx);
//   std::unique_lock skip_lock(engine->memtable.cur_mtx);
//   std::unique_lock frozn_lock(engine->memtable.frozen_mtx);
// 写冲突,
//   if (engine->chech_write(key,tranc_id)) {
//           tran_manager_->add_ready_to_flush_tranc_id(tranc_id,TransactionState::ABORTED);
//           return;
//   }
// vlog
// const auto& threshold = TomlConfig::getInstance().getWisckeyValueThreshold();
// auto value_=std::move(engine->tran_vlog(key,value));
// if (engine->vlog_&&threshold>0&&value.size() >= threshold) {
//            engine->vlog_->sync();
//     }
// auto threshold = TomlConfig::getInstance().getWisckeyValueThreshold();
// bool use_vlog = engine->vlog_ && threshold > 0 && value.size() >= threshold;

// std::string value_ = use_vlog ? engine->tran_vlog(key, value) : value;
// group write
// if (use_vlog) {
//   engine->vlog_->sync();
// }
// wal
//  std::vector<Record> operations;
// operations.emplace_back(Record::createRecord(tranc_id));
//    operations.emplace_back(Record::putRecord(tranc_id, key,value_));
//    operations.emplace_back(Record::commitRecord(tranc_id));
// spdlog在write_to_wal已经有了
//    if (!tran_manager_->write_to_wal(operations)) {
//      spdlog::error(
//          "TranContext--commit(): Failed to write WAL for transaction ID={}",
//          tranc_id);
//      throw std::runtime_error("write to wal failed");
//    }
//  spdlog::info(
//      "TranContext--commit(): Transaction ID={} committed successfully",
//      tranc_id);
//   spdlog::trace("TranContext--commit(): Transaction ID={} committed
//   successfully",
//              tranc_id);
// 写入memtable
//      engine->memtable.put_(key,value_,tranc_id);
// 检测flushed
//       engine->memtable.put_("","",tranc_id);
// 增加skiplist
//       engine->memtable.maybe_frozen_cur_table_();
//    }
//     engine->request_flush();
//      tran_manager_->add_ready_to_flush_tranc_id(tranc_id,TransactionState::OP_COMMITTED);
// }

void LSM::put_batch(
    const std::vector<std::pair<std::string, std::string>> &kvs) {
  // auto tranc_id = tran_manager_->getNextTransactionId();
  // engine->put_batch(kvs, tranc_id);
  // 这里不能如上面的put一般不开启事务，要校验写冲突和崩溃恢复
  auto tranc_id = tran_manager_->getNextTransactionId();

    // vlog
    const auto &threshold =
        TomlConfig::getInstance().getWisckeyValueThreshold();
    bool use_vlog;
    std::string value_;
    std::vector<Record> operations;
   const auto& encoded_kvs = engine->tran_vlog_batch(kvs);

    for (auto &[key, value] : encoded_kvs) {
      operations.emplace_back(Record::putRecord(tranc_id, key, value));
    }

    // operations.emplace_back(Record::createRecord(tranc_id));
    operations.emplace_back(Record::commitRecord(tranc_id));
    if (!tran_manager_->write_to_wal(operations)) {
      spdlog::error(
          "TranContext--commit(): Failed to write WAL for transaction ID={}",
          tranc_id);
      throw std::runtime_error("write to wal failed");
    }
    // spdlog::info(
    //     "TranContext--commit(): Transaction ID={} committed successfully",
    //     tranc_id);
    // spdlog::trace(
    //     "TranContext--commit(): Transaction ID={} committed successfully",
    //     tranc_id);
    bool need_flush=false;
    {
    std::unique_lock skip_lock(engine->memtable.cur_mtx);
    std::unique_lock frozen_lock(engine->memtable.frozen_mtx);
    for (auto &[key, value] : encoded_kvs) {
      // 写入memtable
      engine->memtable.put_(key, value, tranc_id);
    }
    // 检测flushed
    engine->memtable.put_("", "", tranc_id);

    //  增加skiplist
    if (engine->memtable.current_table->get_size() >=
      TomlConfig::getInstance().getLsmPerMemSizeLimit()) {
    engine->memtable.frozen_cur_table_();
    need_flush = true;
     }
  }
  tran_manager_->add_ready_to_flush_tranc_id(tranc_id,
                                             TransactionState::OP_COMMITTED);
  if (need_flush) {
  engine->request_flush();
}
}
void LSM::remove(const std::string &key) {
  auto tranc_id = tran_manager_->getNextTransactionId();
 
    std::vector<Record> operations;
    // operations.emplace_back(Record::createRecord(tranc_id));
    operations.emplace_back(Record::putRecord(tranc_id, key, ""));
    operations.emplace_back(Record::commitRecord(tranc_id));
    if (!tran_manager_->write_to_wal(operations)) {
      spdlog::error(
          "TranContext--commit(): Failed to write WAL for transaction ID={}",
          tranc_id);
      throw std::runtime_error("write to wal failed");
    }
    // spdlog::info(
    //     "TranContext--commit(): Transaction ID={} committed successfully",
    //     tranc_id);
    // spdlog::trace(
    //     "TranContext--commit(): Transaction ID={} committed successfully",
     //   tranc_id);
     bool need_flush = false;
     {
     std::unique_lock skip_lock(engine->memtable.cur_mtx);
     std::unique_lock frozen_lock(engine->memtable.frozen_mtx);
    // 写入memtable
    engine->memtable.put_(key, "", tranc_id);
    // 检测flushed
    engine->memtable.put_("", "", tranc_id);
    //  增加skiplist
    
    if (engine->memtable.current_table->get_size() >=
      TomlConfig::getInstance().getLsmPerMemSizeLimit()) {
    engine->memtable.frozen_cur_table_();
    need_flush = true;
  }
  }
  tran_manager_->add_ready_to_flush_tranc_id(tranc_id,
                                             TransactionState::OP_COMMITTED);
  if (need_flush) {
  engine->request_flush();
}
}
// void LSM::remove(const std::string &key) {
//    auto tranc_id = tran_manager_->getNextTransactionId();
//  {
//   std::unique_lock sst_lock(engine->ssts_mtx);
//   std::unique_lock skip_lock(engine->memtable.cur_mtx);
//   std::unique_lock frozn_lock(engine->memtable.frozen_mtx);
// 写冲突,
//   if (engine->chech_write(key,tranc_id)) {
//           tran_manager_->add_ready_to_flush_tranc_id(tranc_id,TransactionState::ABORTED);
//           return;
//   }
// vlog
// if (engine->vlog_&&TomlConfig::getInstance().getWisckeyValueThreshold()>0) {
//            engine->vlog_->sync();
//     }
// wal
//    std::vector<Record> operations;
//    operations.emplace_back(Record::createRecord(tranc_id));
//    operations.emplace_back(Record::deleteRecord(tranc_id, key));
//    operations.emplace_back(Record::commitRecord(tranc_id));
// spdlog在write_to_wal已经有了
//    if (!tran_manager_->write_to_wal(operations)) {
//      spdlog::error(
//          "TranContext--commit(): Failed to write WAL for transaction ID={}",
//          tranc_id);
//      throw std::runtime_error("write to wal failed");
//    }
//    spdlog::trace(
//        "TranContext--commit(): Transaction ID={} committed successfully",
//        tranc_id);
// 写入memtable
//          engine->memtable.remove_(key,tranc_id);
// 检测flushed
//          engine->memtable.put_("","",tranc_id);
// 增加skiplist
//          engine->memtable.maybe_frozen_cur_table_();
//    }
//    engine->request_flush();
//      tran_manager_->add_ready_to_flush_tranc_id(tranc_id,TransactionState::OP_COMMITTED);
// }

void LSM::remove_batch(const std::vector<std::string> &keys) {
  // auto tranc_id = tran_manager_->getNextTransactionId();
  // engine->remove_batch(keys, tranc_id);
  auto tranc_id = tran_manager_->getNextTransactionId();

    std::vector<Record> operations;
    for (auto &key : keys) {
      operations.emplace_back(Record::putRecord(tranc_id, key, ""));
    }
    // operations.emplace_back(Record::createRecord(tranc_id));
    operations.emplace_back(Record::commitRecord(tranc_id));
    if (!tran_manager_->write_to_wal(operations)) {
      spdlog::error(
          "TranContext--commit(): Failed to write WAL for transaction ID={}",
          tranc_id);
      throw std::runtime_error("write to wal failed");
    }
    // spdlog::info(
    //     "TranContext--commit(): Transaction ID={} committed successfully",
    //     tranc_id);
    // spdlog::trace(
    //     "TranContext--commit(): Transaction ID={} committed successfully",
    //     tranc_id);
    bool need_flush = false;
    {
      std::unique_lock skip_lock(engine->memtable.cur_mtx);
      std::unique_lock frozen_lock(engine->memtable.frozen_mtx);
    for (auto &key : keys) {
      // 写入memtable
      engine->memtable.put_(key, "", tranc_id);
    }
    // 检测flushed
    engine->memtable.put_("", "", tranc_id);
    //  增加skiplist
   
    if (engine->memtable.current_table->get_size() >=
      TomlConfig::getInstance().getLsmPerMemSizeLimit()) {
    engine->memtable.frozen_cur_table_();
    need_flush = true;
  }
  }
  tran_manager_->add_ready_to_flush_tranc_id(tranc_id,
                                             TransactionState::OP_COMMITTED);
  if (need_flush) {
  engine->request_flush();
}
}
void LSM::clear() { engine->clear(); }

void LSM::flush() { engine->request_flush(); }

void LSM::flush_all() {
 {
    std::unique_lock cur_lock(engine->memtable.cur_mtx);
    std::unique_lock frozen_lock(engine->memtable.frozen_mtx);

    if (engine->memtable.current_table->get_size() > 0) {
      engine->memtable.frozen_cur_table_();
    }
  }
  while (engine->memtable.get_frozen_size()>0) {
        engine->flush_one_frozens();
  }
  
}

LSM::LSMIterator LSM::begin(uint64_t tranc_id) {
  return engine->begin(tranc_id);
}

LSM::LSMIterator LSM::end() { return engine->end(); }

std::optional<std::pair<TwoMergeIterator, TwoMergeIterator>>
LSM::lsm_iters_monotony_predicate(
    uint64_t tranc_id, std::function<int(const std::string &)> predicate) {
  return engine->lsm_iters_monotony_predicate(tranc_id, predicate);
}

// 开启一个事务
std::shared_ptr<TranContext>
LSM::begin_tran(const IsolationLevel &isolation_level) {
  auto tranc_context = tran_manager_->new_tranc(isolation_level);

  spdlog::info("LSM--"
               "lsm_iters_monotony_predicate: Starting query for tranc_id={}",
               tranc_context->tranc_id_);

  return tranc_context;
}

void LSM::set_log_level(const std::string &level) { reset_log_level(level); }
} // namespace tiny_lsm
