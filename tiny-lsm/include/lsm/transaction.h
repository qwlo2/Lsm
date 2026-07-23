#pragma once

#include "utils/files.h"
#include "wal/record.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tiny_lsm {

enum class IsolationLevel {
  READ_UNOP_COMMITTED,
  READ_OP_COMMITTED,
  REPEATABLE_READ,
  SERIALIZABLE
};

enum class TransactionState {
  OP_COMMITTED,
  ABORTED
};

inline std::string isolation_level_to_string(const IsolationLevel &level);

class LSMEngine;
class TranManager;
class WAL;
struct SharedEntryState;

class TranContext {
  friend class TranManager;

public:
  TranContext(uint64_t tranc_id, std::shared_ptr<LSMEngine> engine,
              std::shared_ptr<TranManager> tranManager,
              const enum IsolationLevel &isolation_level=IsolationLevel::REPEATABLE_READ);
  void put(const std::string &key, const std::string &value);
  void remove(const std::string &key);
  std::optional<std::string> get(const std::string &key);

  // ! test_fail = true 是测试中手动触发的崩溃
  bool commit(bool test_fail = false);
  bool abort();
  enum IsolationLevel get_isolation_level();

public:
  std::shared_ptr<LSMEngine> engine_;
  std::weak_ptr<TranManager> tranManager_;
  uint64_t tranc_id_;
  std::vector<Record> operations;//操作类型,用于wal
  std::unordered_map<std::string, std::string> temp_map_;
  //put（a,"")和remove没有区分开
    bool isCommited = false;
  bool isAborted = false;
  enum IsolationLevel isolation_level_;//隔离级别

private:
  // RU 事务写入 MemTable 的所有 pending Entry 共享这个状态。
  // commit/abort 只需修改一次，即可使这些 Entry 整体失效。
  std::shared_ptr<SharedEntryState> pending_state_;

  std::unordered_map<std::string,
                     std::optional<std::pair<std::string, uint64_t>>>
      read_map_;//读缓存
  std::unordered_map<std::string,
                     std::optional<std::pair<std::string, uint64_t>>>
      rollback_map_;//修改前的值
      //可回滚单条或多条，savepoint，不实现，太麻烦
      //事务结束的标志为提交或回滚，回滚后不能再做任何操作
      //只用于ru
};

class TranManager : public std::enable_shared_from_this<TranManager> {
public:
  TranManager(std::string data_dir);
  ~TranManager();
  void init_new_wal();
  void set_engine(std::shared_ptr<LSMEngine> engine);
  std::shared_ptr<TranContext> new_tranc(const IsolationLevel &isolation_level);

  uint64_t getNextTransactionId();
  // 普通读取使用当前可见上界，不额外分配事务 ID。
  uint64_t getCurrentReadId() const {
    return nextTransactionId_.load(std::memory_order_acquire);
  }
  uint64_t get_max_flushed_tranc_id();
  uint64_t get_checkpoint_tranc_id();
  uint64_t get_oldest_active_tranc_id();
  std::set<uint64_t>& get_flushed_tranc_ids();

  void add_ready_to_flush_tranc_id(uint64_t tranc_id, TransactionState state);
  void add_flushed_tranc_ids(const std::vector<uint64_t>& ids);
  // void remove_active_tranc_id(uint64_t tranc_id);

  bool write_to_wal(const std::vector<Record> &records);

  std::map<uint64_t, std::vector<Record>> check_recover();
  std::size_t check_recover(
      const std::function<void(uint64_t, std::vector<Record> &&)>
          &recover_callback);

  std::string get_tranc_id_file_path();
  void write_tranc_id_file();
  void read_tranc_id_file();
  // void flusher();

private:
  mutable std::mutex mutex_;//wal，nextTransactionId_（多线程单个变量不需要）
  std::shared_ptr<LSMEngine> engine_;
  std::shared_ptr<WAL> wal;//只对commit的做记录
  std::string data_dir_;
  // std::atomic<bool> flush_thread_running_ = true;
  std::atomic<uint64_t> nextTransactionId_ = 1;
  std::map<uint64_t, std::shared_ptr<TranContext>> activeTrans_;
  std::map<uint64_t, TransactionState> readyToFlushTrancIds_;
  std::set<uint64_t> flushedTrancIds_;
  FileObj tranc_id_file_;
};

} // namespace tiny_lsm
