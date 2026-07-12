#pragma once

#include "memtable/memtable.h"
#include "sst/sst.h"
#include "compact.h"
#include "transaction.h"
#include "two_merge_iterator.h"
#include "vlog/vlog.h"
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace tiny_lsm {

class Level_Iterator;

class LSMEngine : public std::enable_shared_from_this<LSMEngine> {
  friend class Mentable;
public:
  std::string data_dir;
  MemTable memtable;
  std::map<size_t, std::deque<size_t>> level_sst_ids;
  std::unordered_map<size_t, std::shared_ptr<SST>> ssts;
  std::shared_mutex ssts_mtx;
  std::shared_ptr<BlockCache> block_cache;
  std::shared_ptr<VLog> vlog_;
  std::weak_ptr<TranManager> tran_manager;
 std::atomic<size_t> next_sst_id{0};
  size_t cur_max_level = 0;
  //对wisckey的修改
  uint8_t storage_mode_ = 0;

  //flush异步
  std::thread flush_thread_;
  std::mutex flush_mtx_;
  std::condition_variable flush_cv_;
  std::atomic<bool> flush_stop_{false};
  bool flush_requested_{false};
   std::mutex flush_job_mtx_;
  //compact异步
  std::thread compact_thread_;
  std::mutex compact_mtx_;
  std::condition_variable compact_cv_;
  std::atomic<bool> compact_stop_{false};
  bool compact_requested_{false};
  
  

public:
  LSMEngine(std::string path);
  ~LSMEngine();
   //对vlog的转换
  std::string tran_vlog(const std::string& key,const std::string& value);
  std::vector<std::pair<std::string, std::string>>
  tran_vlog_batch(const std::vector<std::pair<std::string, std::string>> &kvs);
  //加的检查写冲突的函数
  bool chech_write(const std::string &key,const uint64_t& tranc_id_);
  //将resolve移动到这里，sst，memtable的返回值需要解码，sst的resolve被注释
  std::string resolve_value_try(const std::string &raw_value) const;

  std::optional<std::pair<std::string, uint64_t>> get(const std::string &key,
                                                      uint64_t tranc_id);
  std::vector<
      std::pair<std::string, std::optional<std::pair<std::string, uint64_t>>>>
  get_batch(const std::vector<std::string> &keys, uint64_t tranc_id);

  std::optional<std::pair<std::string, uint64_t>>
  sst_get_(const std::string &key, uint64_t tranc_id);

  // 如果触发了刷盘, 返回当前刷入sst的最大事务id
  uint64_t put(const std::string &key, const std::string &value,
               uint64_t tranc_id);
  
  uint64_t
  put_batch(const std::vector<std::pair<std::string, std::string>> &kvs,
            uint64_t tranc_id);

  uint64_t remove(const std::string &key, uint64_t tranc_id);
  uint64_t remove_batch(const std::vector<std::string> &keys,
                        uint64_t tranc_id);
  void clear();
//  uint64_t flush();

  std::string get_sst_path(size_t sst_id, size_t target_level);

  std::optional<std::pair<TwoMergeIterator, TwoMergeIterator>>
  lsm_iters_monotony_predicate(
      uint64_t tranc_id, std::function<int(const std::string &)> predicate);

  Level_Iterator begin(uint64_t tranc_id);
  Level_Iterator end();

  static size_t get_sst_size(size_t level);

  void set_tran_manager(std::shared_ptr<TranManager> tran_manager);
//flush异步
  void start_flush_thread();
  void stop_flush_thread();
  void flush_worker();
  void request_flush();
  void flush_one_frozens();
  //compact
  void start_compact_thread();
  void stop_compact_thread();
  void request_compact(size_t level);
  void compact_worker();

private:
  void full_compact(size_t src_level);
  // std::vector<std::shared_ptr<SST>>
  // full_l0_l1_compact(std::vector<size_t> &l0_ids, std::vector<size_t> &l1_ids);

  // std::vector<std::shared_ptr<SST>>
  // full_common_compact(std::vector<size_t> &lx_ids, std::vector<size_t> &ly_ids,
  //                     size_t level_y);
std::vector<std::shared_ptr<SST>>
full_l0_l1_compact(const std::vector<std::shared_ptr<SST>> &l0_ssts,
                   const std::vector<std::shared_ptr<SST>> &l1_ssts);

std::vector<std::shared_ptr<SST>>
full_common_compact(const std::vector<std::shared_ptr<SST>> &lx_ssts,
                    const std::vector<std::shared_ptr<SST>> &ly_ssts,
                    size_t level_y);
  std::vector<std::shared_ptr<SST>> gen_sst_from_iter(BaseIterator &iter,
                                                      size_t target_sst_size,
                                                      size_t target_level);

};

class LSM {
private:
  std::shared_ptr<LSMEngine> engine;
  std::shared_ptr<TranManager> tran_manager_;

public:
  LSM(std::string path);
  ~LSM();

  std::optional<std::string> get(const std::string &key);
  std::vector<std::pair<std::string, std::optional<std::string>>>
  get_batch(const std::vector<std::string> &keys);

  void put(const std::string &key, const std::string &value);
  void put_batch(const std::vector<std::pair<std::string, std::string>> &kvs);

  void remove(const std::string &key);
  void remove_batch(const std::vector<std::string> &keys);

  using LSMIterator = Level_Iterator;
  LSMIterator begin(uint64_t tranc_id);
  LSMIterator end();
  std::optional<std::pair<TwoMergeIterator, TwoMergeIterator>>
  lsm_iters_monotony_predicate(
      uint64_t tranc_id, std::function<int(const std::string &)> predicate);
  void clear();
  void flush();
  void flush_all();

  // 开启一个事务
  std::shared_ptr<TranContext>
  begin_tran(const IsolationLevel &isolation_level);

  // 重设日志级别
  void set_log_level(const std::string &level);
};
} // namespace tiny_lsm