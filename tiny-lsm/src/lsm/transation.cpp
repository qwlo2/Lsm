#include "config/config.h"
#include "lsm/engine.h"
#include "lsm/transaction.h"
#include "sst/sst.h"
#include "utils/files.h"
#include "utils/set_operation.h"
#include "spdlog/spdlog.h"
#include "vlog/vlog.h"
#include "wal/record.h"
#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stdatomic.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <unordered_set>
namespace tiny_lsm {

inline std::string isolation_level_to_string(const IsolationLevel &level) {
  switch (level) {
  case IsolationLevel::READ_UNOP_COMMITTED:
    return "READ_UNOP_COMMITTED";
  case IsolationLevel::READ_OP_COMMITTED:
    return "READ_OP_COMMITTED";
  case IsolationLevel::REPEATABLE_READ:
    return "REPEATABLE_READ";
  case IsolationLevel::SERIALIZABLE:
    return "SERIALIZABLE";
  default:
    return "UNKNOWN";
  }
}

// *********************** TranContext ***********************
TranContext::TranContext(uint64_t tranc_id, std::shared_ptr<LSMEngine> engine,
                         std::shared_ptr<TranManager> tranManager,
                         const enum IsolationLevel &isolation_level)
 :tranc_id_(tranc_id),engine_(engine),tranManager_(tranManager),isolation_level_(isolation_level){}
//为了支持事务回滚，在put就把大小值分离
//ru会直接进入memtable，rc，rr会进入temp——section，存储所有put的kv，为commit做准备
//都会对当前的key进行一次get，记录当前可见到的最新值，写入operations，wal
//在rr级别，在读取时会写入read——sectio，做读缓存，实现可重复度读
void TranContext::put(const std::string &key, const std::string &value) {
    //vlog,ru进入memtable进行vlog-tran，temp-map存原值，operation存引用
    auto vt=TomlConfig::getInstance().getWisckeyValueThreshold();
    std::string value_;
    if (engine_->vlog_ && vt > 0 &&  value.size() >= vt) {
        value_=std::move(engine_->tran_vlog(key,value));  
    }else {
       value_=value;
    }
        //auto  value_=std::move(engine_->tran_vlog(key,value));  
       if (isolation_level_==IsolationLevel::READ_UNOP_COMMITTED) {
             std::unique_lock sst_lock(engine_->ssts_mtx);
           std::unique_lock skip_lock(engine_->memtable.cur_mtx);
           std::unique_lock frozn_lock(engine_->memtable.frozen_mtx);
           //读到的也是vlog处理过的
           
           auto save=engine_->memtable.get_(key,tranc_id_);
            if (engine_->chech_write(key,tranc_id_)) {
                  abort();
                  return;
            }  
            engine_->memtable.put_(key, value_,  tranc_id_);
           // engine_->memtable.maybe_frozen_cur_table_();
            //roll_back是加入delete
              //RU下这里才记录，rc及以上，只需clear，因为没有实现。savepoint
              if (save.get_value().empty()) {
                rollback_map_.emplace(key,std::nullopt);
              }else {
                //此时为未删除与删除
                 rollback_map_.emplace(key,std::make_pair(save.get_value(),save.get_tranc_id()));
              }
       }else {
          temp_map_[key]=value_;
          
       }
       //若ru以上，此时可能记录的是txn-id大于本身的，后面会abort，因此不影响
         operations.emplace_back(Record::putRecord(tranc_id_,key,value_));
}

void TranContext::remove(const std::string &key) {
    // auto save=std::move(engine_->get(key,tranc_id_));
       if (isolation_level_==IsolationLevel::READ_UNOP_COMMITTED) {
             std::unique_lock sst_lock(engine_->ssts_mtx);
           std::unique_lock skip_lock(engine_->memtable.cur_mtx);
           std::unique_lock frozn_lock(engine_->memtable.frozen_mtx);
         auto save=std::move(engine_->memtable.get_(key,tranc_id_));
             if (engine_->chech_write(key,tranc_id_)) {
                   abort();
                   return;
             }
               engine_->memtable.remove_(key,tranc_id_);
              //
              //  engine_->memtable.maybe_frozen_cur_table_();
             if (save.get_value().empty()) {
                rollback_map_.emplace(key,std::nullopt);
              }else {
               
                 rollback_map_.emplace(key,std::make_pair(save.get_value(), save.get_tranc_id()));
              }
       }else {
          temp_map_[key]="";
       }
       operations.emplace_back(Record::deleteRecord(tranc_id_,key));

}

std::optional<std::string> TranContext::get(const std::string &key) {
    if (auto it = temp_map_.find(key);it != temp_map_.end()) {
      //对于rc以上先在tem—map中寻找
      //空字符和remve区分
                 if (it->second.empty()) {
                   return std::nullopt;
                  }
                   //return it->second;
                   return engine_->resolve_value_try(it->second);
    }
    //对于在commit中寻找，由于没有read——view因此对第一次读取的进行缓存
    if (read_map_.contains(key)) {
       if (!read_map_[key]) {
          return std::nullopt;
       }
       return  read_map_[key]->first;
       //return  engine_->resolve_value_try(read_map_[key]->first);
    }
    uint64_t txn;
    if (isolation_level_==IsolationLevel::READ_UNOP_COMMITTED) {
             txn=0;
    }else {
        txn=tranc_id_;
    }
    //get已经resolve
    auto value=engine_->get(key, txn);
    if (isolation_level_==IsolationLevel::REPEATABLE_READ) {
          read_map_[key]=value;
    }
    if (value.has_value()) {
    spdlog::trace("TranContext--{}: get({}) returned value={}",
                  isolation_level_to_string(isolation_level_), key,
                  value->first);
  } else {
    spdlog::trace("TranContext--{}: get({}) returned no value",
                  isolation_level_to_string(isolation_level_), key);
  }

      return  value.has_value()?std::make_optional(value->first):std::nullopt;
}

bool TranContext::commit(bool test_fail) {
     //std::vector<std::pair<std::string,std::string>> kvs(temp_map_.size());
         std::unique_lock sst_lock(engine_->ssts_mtx);
          std::unique_lock skip_lock(engine_->memtable.cur_mtx);
           std::unique_lock frozn_lock(engine_->memtable.frozen_mtx);
      if (get_isolation_level()!=IsolationLevel::READ_UNOP_COMMITTED) {
           //要从memtable和sst分别搜索
           for (auto& it:temp_map_){
               if (engine_->chech_write(it.first, tranc_id_)) {
                     abort();
                     return false;
              }
           }
        }
             //0，应该用最大seq
            //   auto get_mem= engine_->memtable.get_(it.first,0);
               //memtable中找到且冲突
            //   if (get_mem.is_valid()&&get_mem.get_tranc_id()>tranc_id_) {
             
            //      auto key = it.first;
            //        abort();
            //       spdlog::warn("TranContext--commit(): Conflict detected on key={}, "
            //          "aborting transaction ID={}",
            //         key, tranc_id_);
            //          return false;
            //       }
               //没找到，在sst中找
            //  if (get_mem.is_end()) {
            //    if (tranManager_.lock()->get_max_flushed_tranc_id() <= tranc_id_) {
                   // sst 中最大的 tranc_id 小于当前 tranc_id, 没有冲突
            //             continue;
            //         }
            //          auto get_sst=engine_->sst_get_(it.first,0);
            //     if (get_sst&&get_sst.value().second>tranc_id_) {
            //        auto key=it.first;
            //           abort();
            //          spdlog::warn("TranContext--commit(): Conflict detected on key={}, "
            //                "aborting transaction ID={}",
            //                     key, tranc_id_);
            //                       return false;
            //       }
           // kvs.emplace_back(it);
      //vlog落盘，保证wal有数据，批量写入
      // if (engine_->vlog_&&TomlConfig::getInstance().getWisckeyValueThreshold()>0) {
      //        engine_->vlog_->sync();
      // }
     //wal，事务结束的标志，在memtable还要插入（""",""),以供判断是否flushed，add_flushed_tranc_id
     //temp-map不转化的话，wal也不转化，在这里统一转化
      // for (auto &it : temp_map_) {
      //   it.second = engine_->tran_vlog(it.first, it.second);
      //   operations.emplace_back(
      //       Record::putRecord(tranc_id_, it.first, it.second));
      // }
      operations.emplace_back(Record::commitRecord(tranc_id_));
         //spdlog在write_to_wal已经有了
          if(!tranManager_.lock()->write_to_wal(operations) ){
            spdlog::error("TranContext--commit(): Failed to write WAL for transaction ID={}",tranc_id_);
                throw std::runtime_error("write to wal failed");
          }
    spdlog::info(
        "TranContext--commit(): Transaction ID={} committed successfully",
        tranc_id_);
      //事务结束的标志，在flush中判断fluashed的事务
      //不算数据因此不在record中
      //test——fail模拟wal写入，但memtable崩溃，WAL能否恢复
       bool need_flush = false;
      if (!test_fail) {
       if (get_isolation_level()!=IsolationLevel::READ_UNOP_COMMITTED){
           //写入vlog
            for (auto& it : temp_map_) {
                engine_->memtable.put_(it.first, it.second,tranc_id_);
         }
       }
       //put让skiplist个数可以递增
       engine_->memtable.put_("","",tranc_id_);
       if (engine_->memtable.current_table->get_size() >=
           TomlConfig::getInstance().getLsmPerMemSizeLimit()) {
         engine_->memtable.frozen_cur_table_();
         need_flush = true;
       }
        isCommited=true;
       tranManager_.lock()->add_ready_to_flush_tranc_id(tranc_id_,TransactionState::OP_COMMITTED);
      }
       sst_lock.unlock();
      skip_lock.unlock();
      frozn_lock.unlock();
      if (need_flush) {
        engine_->request_flush();
      }
      return true;

}
//abort是终止的标志，roll-bakc是执行动作
bool TranContext::abort() {
       read_map_.clear();
       temp_map_.clear();
       if (isolation_level_==IsolationLevel::READ_UNOP_COMMITTED) {
          //abort的应用场景都有锁
                for (auto& [key,op]:rollback_map_) {
                         if (!op) {
                           // engine_->put(key,"", tranc_id_);
                            engine_->memtable.remove_(key, tranc_id_);
                         }else {
                         engine_->memtable.put_(key,op->first,tranc_id_);
                         }
                }
            engine_->memtable.maybe_frozen_cur_table_();
       }
     rollback_map_.clear();
     operations.clear();
      isAborted=true;
      tranManager_.lock()->add_ready_to_flush_tranc_id(tranc_id_,TransactionState::ABORTED);
      return true;
}

enum IsolationLevel TranContext::get_isolation_level() {
  return isolation_level_;
}

// *********************** TranManager ***********************
TranManager::TranManager(std::string data_dir) : data_dir_(data_dir) {
  auto file_path = get_tranc_id_file_path();
  tranc_id_file_ = FileObj::open(file_path, !std::filesystem::exists(file_path));
  read_tranc_id_file();
}
//it_new_wal不应该清理掉所有文件，在重新启动后，剩下的没有flushed，
// 应该在这里选出最大的seq，初始化wal，其余的等待清理线程处理
void TranManager::init_new_wal() {
  spdlog::info("TranManager--init_new_wal(): Cleaning up old WAL files");

  // TODO: 1 和 4096 应该统一用宏定义
  // 先清理掉所有 wal. 开头的文件, 因为其已经被重放过了
  // for (const auto &entry : std::filesystem::directory_iterator(data_dir_)) {
   //   auto filename = entry.path().filename().string();
   // 
  //   if (filename.rfind("wal.", 0) == 0) {
  //     std::filesystem::remove(entry.path());
  //   }
  // }
  wal = std::make_shared<WAL>(data_dir_, 128, get_checkpoint_tranc_id(), 1,
                              4*1024*1024);
  //flushedTrancIds_.clear();
  //flushedTrancIds_.insert(nextTransactionId_.load() - 1);
  spdlog::info("TranManager--init_new_wal(): New WAL initialized");
}

void TranManager::set_engine(std::shared_ptr<LSMEngine> engine) {
  engine_ = std::move(engine);
}
//崩溃不行
TranManager::~TranManager() { write_tranc_id_file(); }

// relaxed：
//     只保证 atomic 本身原子，不保证其他数据顺序。
//     适合 ID、计数器。

// release：
//     发布数据，保证之前写入不会跑到它后面。

// acquire：
//     获取数据，看到 release 后，也能看到 release 前写入的数据。

// acq_rel：
//     同时 acquire + release。

// seq_cst：
//    应该是忽略内存顺序？

void TranManager::write_tranc_id_file() {
  // 共4个8字节的整型id记录
  // std::atomic<uint64_t> nextTransactionId_;
  // std::atomic<uint64_t> max_flushed_tranc_id_;
  // std::atomic<uint64_t> checkpoint_tranc_id_;
  //保证写入的原子性
  //只在析构写入，奔溃的还欠缺
  std::unique_lock<std::mutex> lock(mutex_);
  auto spshoot=nextTransactionId_.load();
  auto size_64=sizeof(uint64_t);
  auto offset=size_64*2;
  auto flushed_sixe=flushedTrancIds_.size();
  std::vector<uint8_t> buf(offset+size_64*flushed_sixe);
  memcpy(buf.data(),&spshoot,size_64);
  memcpy(buf.data()+size_64,&flushed_sixe,size_64);
  for (auto&it : flushedTrancIds_) {
         memcpy(buf.data()+offset,&it,size_64);
         offset+=size_64;
  }
 
  tranc_id_file_.write(0,buf);
  tranc_id_file_.sync();
}

void TranManager::read_tranc_id_file() {
  std::unique_lock lock(mutex_);
  flushedTrancIds_.clear();
  //初次
  if (tranc_id_file_.size() < sizeof(uint64_t) * 2) {
    nextTransactionId_.store(1);
    flushedTrancIds_.insert(0);
    return;
  }

  auto buf = tranc_id_file_.read_to_slice(0, sizeof(uint64_t) * 2);
  uint64_t next_id;
  memcpy(&next_id, buf.data(), sizeof(uint64_t));
  nextTransactionId_.store(next_id);

  uint64_t flushed_size;
  memcpy(&flushed_size, buf.data() + sizeof(uint64_t), sizeof(uint64_t));
  size_t expected_size = sizeof(uint64_t) * (2 + flushed_size);
  //整个file
  if (tranc_id_file_.size() < expected_size) {
    nextTransactionId_.store(1);
    flushedTrancIds_.insert(0);
    return;
  }

  buf = tranc_id_file_.read_to_slice(0, expected_size);
  for (uint64_t i = 0; i < flushed_size; ++i) {
    uint64_t flushed_id;
    memcpy(&flushed_id, buf.data() + (i + 2) * sizeof(uint64_t),
           sizeof(uint64_t));
    flushedTrancIds_.emplace(flushed_id);
  }
  if (flushedTrancIds_.empty()) {
    flushedTrancIds_.insert(0);
  }
}

void TranManager::add_ready_to_flush_tranc_id(uint64_t tranc_id,
                                              TransactionState state) {
      std::unique_lock lock(mutex_);
      readyToFlushTrancIds_.emplace(tranc_id,state);
      
      activeTrans_.erase(tranc_id);
}

//在memtable的flsh中
void TranManager::add_flushed_tranc_ids(const std::vector<uint64_t>& ids) {
//   std::unique_lock lock(mutex_);
//   std::vector<uint64_t> needRemove;
  // for (auto &[readyId, state] : readyToFlushTrancIds_) {
  //   //由于abort的并没有写入，因此采用这种方式筛选
  //   if (readyId < tranc_id && state == TransactionState::ABORTED) {
  //     flushedTrancIds_.emplace(readyId);
  //     needRemove.emplace_back(readyId);
  //   } else if (readyId == tranc_id) {
  //     flushedTrancIds_.emplace(readyId);
  //     needRemove.emplace_back(readyId);
  //     break;
  //   }
//   }
//   for (auto id : needRemove) {
//     readyToFlushTrancIds_.erase(id);
//   }
//   //从最小值开始的连续区间，只保留这个连续区间的最后一个值
//   //因此可以用来判断wal文件的txn是否完全被刷入
//   flushedTrancIds_ = compressSet<uint64_t>(flushedTrancIds_);
//   lock.unlock();
//   if (wal) {
//     wal->set_checkpoint_tranc_id(get_checkpoint_tranc_id());
// }
  if (ids.empty()) {
    return;
  }

  std::unique_lock lock(mutex_);

  std::unordered_set<uint64_t> flushed_ids(ids.begin(), ids.end());
  uint64_t max_flushed_id = *std::max_element(ids.begin(), ids.end());

  std::vector<uint64_t> needRemove;

  for (auto &[readyId, state] : readyToFlushTrancIds_) {
    if (state == TransactionState::ABORTED && readyId < max_flushed_id) {
      flushedTrancIds_.emplace(readyId);
      needRemove.emplace_back(readyId);
    } else if (flushed_ids.contains(readyId)) {
      flushedTrancIds_.emplace(readyId);
      needRemove.emplace_back(readyId);
    }

    if (readyId > max_flushed_id) {
      break;
    }
  }

  for (auto id : needRemove) {
    readyToFlushTrancIds_.erase(id);
  }

  flushedTrancIds_ = compressSet<uint64_t>(flushedTrancIds_);

  uint64_t checkpoint =
      flushedTrancIds_.empty() ? 0 : *flushedTrancIds_.begin();

  lock.unlock();

  if (wal) {
    wal->set_checkpoint_tranc_id(checkpoint);
  }
}

uint64_t TranManager::getNextTransactionId() {
  //多线程下，atomic前后的普通语句，在cpu哪里可能因为优化，导致atomic和普通语句的执行顺序并不如我们想的那样
  //因此用一些语法强制要求
  //realse：释放，即普通语句玩了，才开始automic
  //acquire：获取
  //这2个如同pv

  return nextTransactionId_.fetch_add(1);
}

std::set<uint64_t> &TranManager::get_flushed_tranc_ids() {
  return flushedTrancIds_;
}

uint64_t TranManager::get_max_flushed_tranc_id() {
  // 需保证 size 至少为1
    std::unique_lock lock(mutex_);
    if (flushedTrancIds_.empty()) {
    return 0;
  }
  return *flushedTrancIds_.rbegin();
}

uint64_t TranManager::get_checkpoint_tranc_id() {
  // 需保证 size 至少为1
    std::unique_lock lock(mutex_);
 if (flushedTrancIds_.empty()) {
    return 0;
  }
  return *flushedTrancIds_.begin();
}
uint64_t TranManager::get_oldest_active_tranc_id() {
  // 需保证 size 至少为1
    std::unique_lock lock(mutex_);
    //无活跃就返回下一个
 if (activeTrans_.empty()) {
    return nextTransactionId_.load();
  }
  return activeTrans_.begin()->first;
}
std::shared_ptr<TranContext>
TranManager::new_tranc(const IsolationLevel &isolation_level) {
  spdlog::debug("TranManager--new_tranc(): Creating new transaction with "
                "isolation level={}",
                static_cast<int>(isolation_level));

  // 获取锁
  std::unique_lock<std::mutex> lock(mutex_);

  auto tranc_id = getNextTransactionId();
 auto tmp = std::make_shared<TranContext>(
      tranc_id, engine_, shared_from_this(), isolation_level);
  activeTrans_.emplace(tranc_id,tmp);
  spdlog::debug("TranManager--new_tranc(): Created transaction ID={} with "
                "isolation level={}",
                tranc_id, static_cast<int>(isolation_level));
  //事务开始的标志
  tmp->operations.emplace_back(Record::createRecord(tranc_id));
  return tmp;
}
std::string TranManager::get_tranc_id_file_path() {
  if (data_dir_.empty()) {
    data_dir_ = "./";
  }
  return data_dir_ + "/tranc_id";
}

std::map<uint64_t, std::vector<Record>> TranManager::check_recover() {
  spdlog::info("TranManager--check_recover(): Starting recovery from WAL");

  std::map<uint64_t, std::vector<Record>> wal_records =
       std::move(WAL::recover(data_dir_, *flushedTrancIds_.begin()));
    

  spdlog::info("TranManager--check_recover(): Recovered {} transactions",
               wal_records.size());

  return wal_records;
}

bool TranManager::write_to_wal(const std::vector<Record> &records) {
  spdlog::trace("TranManager--write_to_wal(): Writing {} records to WAL",
                records.size());

  try {
    //批量git
    wal->log(records, false);
  } catch (const std::exception &e) {
    spdlog::error("TranManager--write_to_wal(): Exception occurred: {}",
                  e.what());

    return false;
  }

  spdlog::trace(
      "TranManager--write_to_wal(): Successfully wrote {} records to WAL",
      records.size());

  return true;
}

// void TranManager::flusher() {
//   while (flush_thread_running_.load()) {
//     std::this_thread::sleep_for(std::chrono::seconds(1));
//     write_tranc_id_file();
//   }
// }
 } // namespace tiny_lsm