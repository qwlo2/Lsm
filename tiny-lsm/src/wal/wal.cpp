// src/wal/wal.cpp

#include "wal/wal.h"
#include <algorithm>
#include <chrono>
#include <functional>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tiny_lsm {

// 从零开始的初始化流程
WAL::WAL(const std::string &log_dir, size_t buffer_size,
         uint64_t checkpoint_tranc_id, uint64_t clean_interval,
         uint64_t file_size_limit)
    : buffer_size_(buffer_size), checkpoint_tranc_id_(checkpoint_tranc_id),
      stop_cleaner_(false), clean_interval_(clean_interval),
      file_size_limit_(file_size_limit) {
  // TODO: Lab 5.4 实现WAL的初始化流程
  // ? 1. 设置 active_log_path_ = log_dir + "/wal.0"
  // ? 2. 用 FileObj::open(active_log_path_, true) 打开或创建 WAL 文件
  // ? 3. 启动清理线程: cleaner_thread_ = std::thread(&WAL::cleaner, this)
  uint64_t max_seq=0;
  bool has_wal=false;
  for (const auto& entry : std::filesystem::directory_iterator(log_dir)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    auto filename = entry.path().filename().string();
    if (filename.rfind("wal.", 0) != 0) {
      continue;
    }
      uint64_t seq = std::stoull(filename.substr(4));
      max_seq = std::max(max_seq, seq);
      has_wal=true;
  }
  //可以复用，但是为了更清晰，新开一个file
  max_seq=has_wal?max_seq+1:0;
  active_log_path_=log_dir+"/wal."+std::to_string(max_seq );
  log_file_=FileObj::open(active_log_path_,!std::filesystem::exists(active_log_path_));
    log_buffer_.reserve(buffer_size_);
    cleaner_thread_ = std::thread(&WAL::cleaner,this);
  //  cleaner_thread_=std::thread([this,&log_dir](){
  //       std::unique_lock lock(mutex_);
  //   std::condition_variable cv;
  //   while (!stop_cleaner_) {
  //       cv.wait(lock,[this,&log_dir](){
  //          return  !std::filesystem::is_empty(log_dir)||stop_cleaner_;
  //   }); 
  //    if (stop_cleaner_) {
  //       return ;
  //    }
  //     cleanWALFile();
  //   }
  //  } );

  
}
//崩溃时，不一定会析构
WAL::~WAL() {
  // TODO: Lab 5.4 实现WAL的清理流程
  // ? 1. 强制将缓冲区所有内容刷盘: log({}, true)
  // ? 2. 加锁设置 stop_cleaner_ = true
  // ? 3. 等待清理线程结束: cleaner_thread_.join()
  // ? 4. 显式关闭文件: log_file_.close()
  try {
    flush();
  } catch (const std::exception &err) {
    std::cerr << "WAL flush during destruction failed: " << err.what()
              << std::endl;
  } catch (...) {
    std::cerr << "WAL flush during destruction failed: unknown exception"
              << std::endl;
  }
  //此时若不加{}，cleanWALFile()和这里会死锁
  {
    std::unique_lock lock(mutex_);
    stop_cleaner_=true;
  }
   if (cleaner_thread_.joinable()) {
       cleaner_thread_.join();
   }
   try {
     log_file_.close();
   } catch (const std::exception &err) {
     std::cerr << "WAL close during destruction failed: " << err.what()
               << std::endl;
   } catch (...) {
     std::cerr << "WAL close during destruction failed: unknown exception"
               << std::endl;
   }
}

size_t WAL::recover_each(const std::string &log_dir,
                         uint64_t checkpoint_tranc_id,
                         const RecoverCallback &recover_callback) {
  if (!std::filesystem::exists(log_dir) ||
      std::filesystem::is_empty(log_dir)) {
    return 0;
  }

  std::vector<std::pair<uint64_t, std::string>> wal_seq;
  for (const auto &entry : std::filesystem::directory_iterator(log_dir)) {
    if (!entry.is_regular_file()) {
      continue;
    }

    auto filename = entry.path().filename().string();
    if (filename.rfind("wal.", 0) != 0) {
      continue;
    }

    auto seq = std::stoull(filename.substr(4));
    wal_seq.emplace_back(seq, entry.path().string());
  }

  std::sort(wal_seq.begin(), wal_seq.end(),
            [](const auto &lhs, const auto &rhs) {
              return lhs.first < rhs.first;
            });

  std::unordered_map<uint64_t, std::vector<Record>> pending;
  size_t recovered_count = 0;

  for (const auto &wal : wal_seq) {
    auto file = FileObj::open(wal.second, false);
    auto decoded = Record::decode(file.read_to_slice(0, file.size()));

    for (auto &record : decoded) {
      const uint64_t tranc_id = record.getTrancId();
      if (tranc_id <= checkpoint_tranc_id) {
        continue;
      }

      const auto operation = record.getOperationType();
      if (operation == OperationType::OP_ROLLBACK) {
        pending.erase(tranc_id);
        continue;
      }

      auto &records = pending[tranc_id];
      records.emplace_back(std::move(record));

      if (operation == OperationType::OP_COMMIT) {
        auto committed_records = std::move(records);
        pending.erase(tranc_id);
        recover_callback(tranc_id, std::move(committed_records));
        ++recovered_count;
      }
    }
  }

  return recovered_count;
}

std::map<uint64_t, std::vector<Record>>
WAL::recover(const std::string &log_dir, uint64_t checkpoint_tranc_id) {
  // TODO: Lab 5.5 检查需要重放的WAL日志
  // ? 1. 若 log_dir 不存在则直接返回空 map
  // ? 2. 遍历目录找到所有 "wal." 前缀的文件
  // ? 3. 按 seq 升序排序
  // ? 4. 逐文件读取所有 Record (Record::decode)
  // ?    仅保留 tranc_id > checkpoint_tranc_id 的记录
  // ? 5. 返回 map<tranc_id, records>
  if (std::filesystem::is_empty(log_dir)) {
         return {};
  }
  std::vector<std::pair<int,std::string>> wal_seq;
  //std::filesystem::directory_iterator就像memtable的堆迭代器一样
  for (auto& entry : std::filesystem::directory_iterator(log_dir)) {
        if (entry.is_regular_file()) {
           //path是string的封装
           //extension返回filename最后一个.后面的string，stem则是.之前的filename的string
           //extension会带.,stem不会
           auto filename = entry.path().filename().string();
           //filename.starts_with("wal."
            if (filename.rfind("wal.",0)==0) {
                auto seq=std::stoull(filename.substr(4));
                 wal_seq.emplace_back(seq,entry.path().string());
            }
        }
  }
  //先快排，递归过深用桶排序，大小小于16用插入排序
  //sort的比较函数必须要有2个参数
  //函数只需要返回true或false，是模板的概念
  //当<时为升序，>时为降序
  std::sort(wal_seq.begin(),wal_seq.end(),[](const auto& a,const auto& b){
       return  a.first<b.first;
  });
  std::map<uint64_t, std::vector<Record>> ans;
    std::unordered_map<uint64_t, bool> committed;
  for (auto& it : wal_seq){
        //相对路径会在当前目录拼接上绝对路径进行查找
         auto file=FileObj::open(it.second,false);
        //const& 能接右值，是为了安全地引用临时对象
        //&绑定右值可能会修改它，而右值不稳定，因此不行
         auto de_vec=Record::decode(file.read_to_slice(0,file.size()));
      //    auto n=de_vec.size();
      //    uint64_t tmp=de_vec[0].getTrancId();
      //    std::vector<Record> tmp_vec;
      //   for (int i=0; i<n;++i) {
      //        if (tmp==de_vec[i].getTrancId()) {
      //              tmp_vec.emplace_back(de_vec[i]);
      //        }else if (tmp_vec.back().getOperationType()==OperationType::OP_COMMIT
      //                   &&tmp_vec.back().getTrancId()>checkpoint_tranc_id) {
      //             ans.emplace(tmp,tmp_vec);
      //             tmp_vec={};
      //        }else {
      //            tmp_vec={};
      //        }
      //        tmp=de_vec[i].getTrancId();
      //   }
      //  if (tmp_vec.back().getOperationType()==OperationType::OP_COMMIT
      //                   &&tmp_vec.back().getTrancId()>checkpoint_tranc_id) {
      //           ans.emplace(tmp,tmp_vec);
      //       }
//全部empalce,committed来判断是否留下
for (auto& record : de_vec) {
    auto txn_id = record.getTrancId();
    if (txn_id <= checkpoint_tranc_id) continue;

    ans[txn_id].emplace_back(record);

    if (record.getOperationType() == OperationType::OP_COMMIT) {
        committed[txn_id] = true;
    }
}
  }
//会删除元素不用for：
for (auto item = ans.begin(); item != ans.end(); ) {
    if (!committed[item->first]) {
        item = ans.erase(item);
    } else {
        ++item;
    }
  }
//这里把事务的recoed分离，依托chechpoint，这个崩溃好像不保险
//record是每一共有len+txn+op，整个文件全部是record，因此可能要一个个的区分txn
//可能还要判断最后一个是不是commit，表示则抛弃；
 return ans;
}

// commit 时强制写入
void WAL::flush() {
  // TODO: Lab 5.4 强制刷盘
  // ? 当前实现仅需加锁保证当前写入完成即可
  // ? 若 log() 中使用了缓冲区, 这里需要确保缓冲区内容全部落盘
  std::lock_guard<std::mutex> lock(mutex_);
  if (!log_buffer_.empty()) {
        // uint64_t txn_max=0;
    for (auto& it :log_buffer_) {
      // txn_max=std::max(txn_max,it.getTrancId());
       auto en=std::move(it.encode());
         if (!log_file_.append(en)) {
           throw std::runtime_error("append wal record failed");
      }
    }
    /// log_file_.append_uint64(txn_max);
  }
if (!log_file_.sync()) {
    throw std::runtime_error("sync wal failed");
}
  if (!stop_cleaner_&&log_file_.size()>=file_size_limit_) {
      //创造新文件
          reset_file();
    }
    log_buffer_.clear();
}

void WAL::set_checkpoint_tranc_id(uint64_t checkpoint_tranc_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  checkpoint_tranc_id_ = checkpoint_tranc_id;
}

void WAL::log(const std::vector<Record> &records, bool force_flush) {
  // TODO: Lab 5.4 实现WAL的写入流程
  // ? 1. 加锁
  // ? 2. 将 records 追加到 log_buffer_
  // ? 3. 若 log_buffer_.size() < buffer_size_ 且 !force_flush 则直接返回
  // ? 4. 否则将 log_buffer_ 中所有记录编码并写入 log_file_ (record.encode())
  // ? 5. 调用 log_file_.sync() 确保落盘
  // ? 6. 若文件大小超过 file_size_limit_ 则调用 reset_file() 滚动日志文件
    std::unique_lock lock(mutex_);
    for (auto& it :records) {
          log_buffer_.emplace_back(it);
    }
    if (log_buffer_.size()<buffer_size_&&!force_flush) {
          return;
    }
    //找到最大txn-id,并append
    //uint64_t txn_max=0;
    for (auto& it :log_buffer_) {
       //txn_max=std::max(txn_max,it.getTrancId());
       auto en=std::move(it.encode());
        if (!log_file_.append(en)) {
           throw std::runtime_error("append wal record failed");
         }
    }
      //log_file_.append_uint64(txn_max);
    //flush
    if (!log_file_.sync()) {
    throw std::runtime_error("sync wal failed");
  }
    if (log_file_.size()>=file_size_limit_) {
      //创造新文件
          reset_file();
    }
    log_buffer_.clear();
}

void WAL::cleaner() {
  // TODO: Lab 5.4 实现WAL的清理线程
  // ? 循环:
  // ?   1. sleep clean_interval_ 秒
  // ?   2. 若 stop_cleaner_ 为 true 则退出
  // ?   3. 调用 cleanWALFile() 清理已可以删除的旧 WAL 文件
   while (!stop_cleaner_) {
      //sleep(clean_interval_);
      std::this_thread::sleep_for(std::chrono::seconds(clean_interval_));
     if (stop_cleaner_) {
        return;
     }
     try {
       cleanWALFile();
     } catch (const std::exception &err) {
       std::cerr << "WAL cleaner failed: " << err.what() << std::endl;
     } catch (...) {
       std::cerr << "WAL cleaner failed: unknown exception" << std::endl;
     }
     
   }
   
}

void WAL::cleanWALFile() {
  // 遍历log_file_所在的文件夹
  std::string dir_path;

  std::unique_lock<std::mutex> lock(mutex_); // 只在获取当前文件路径时获取锁
  if (active_log_path_.find("/") != std::string::npos) {
    dir_path =
        active_log_path_.substr(0, active_log_path_.find_last_of("/")) + "/";
  } else {
    dir_path = "./";
  }
  lock.unlock();

  // wal文件格式为:
  // wal.seq

  std::vector<std::pair<size_t, std::string>> wal_paths;

  for (const auto &entry : std::filesystem::directory_iterator(dir_path)) {
    if (entry.is_regular_file() &&
        entry.path().filename().string().substr(0, 4) == "wal.") {
      std::string filename = entry.path().filename().string();
      size_t dot_pos = filename.find_last_of(".");
      std::string seq_str = filename.substr(dot_pos + 1);
      uint64_t seq = std::stoull(seq_str);
      wal_paths.push_back({seq, entry.path().string()});
    }
  }

  // 按照seq升序排序
  std::sort(wal_paths.begin(), wal_paths.end(),
            [](const std::pair<size_t, std::string> &a,
               const std::pair<size_t, std::string> &b) {
              return a.first < b.first;
            });

  //判断是否可以删除
  std::vector<FileObj> del_paths;
  for (int idx = 0; idx < (int)wal_paths.size() - 1; idx++) {
    auto cur_path = wal_paths[idx].second;
    auto cur_file = FileObj::open(cur_path, false);
    //遍历文件记录, 读取所有的tranc_id,
    //判断是否都小于等于checkpoint_tranc_id_
    size_t offset = 0;
    bool has_unfinished = false;
    while (offset + sizeof(uint16_t) < cur_file.size()) {
      uint16_t record_size = cur_file.read_uint16(offset);
      uint64_t tranc_id = cur_file.read_uint64(offset + sizeof(uint16_t));
      if (tranc_id > checkpoint_tranc_id_) {
        has_unfinished = true;
        break;
      }
      offset += record_size;
    }
    if (!has_unfinished) {
      del_paths.push_back(std::move(cur_file));
    }  
  }
//    std::vector<FileObj> del_paths;
//    uint64_t max_txn_file=0;
//    for (int idx = 0; idx < (int)wal_paths.size() - 1;++idx) {
//        auto cur_path = wal_paths[idx].second;
//        auto cur_file = FileObj::open(cur_path, false);
//        auto txn_id=cur_file.read_uint64(cur_file.size()-sizeof(uint64_t));
//        if (txn_id<checkpoint_tranc_id_) {
//             del_paths.emplace_back(std::move(cur_file));
//        }
//  }
  for (auto &del_file : del_paths) {
    del_file.del_file();
  }
}

void WAL::reset_file() {
  // wal文件格式为:
  // wal.seq
  // 当当前wal文件容量超出阈值后, 创建新的文件, 将seq自增

  auto old_path = active_log_path_;
  // 字符串处理获取seq
  auto seq = std::stoi(old_path.substr(old_path.find_last_of(".") + 1));
  seq++;

  active_log_path_ = old_path.substr(0, old_path.find_last_of(".")) + "." +
                     std::to_string(seq);

  // 创建新的文件
  log_file_ = FileObj::create_and_write(active_log_path_, {});
}
} // namespace tiny_lsm
