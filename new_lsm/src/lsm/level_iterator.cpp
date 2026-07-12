#include "lsm/level_iterator.h"
#include "lsm/engine.h"
#include "memtable/memtable.h"
#include "sst/concact_iterator.h"
#include "sst/sst.h"
#include "sst/sst_iterator.h"
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

// TODO: 需要进行单元测试
namespace tiny_lsm {
Level_Iterator::Level_Iterator(std::shared_ptr<LSMEngine> engine,
                               uint64_t max_tranc_id)
    : engine_(engine), max_tranc_id_(max_tranc_id), rlock_(engine_->ssts_mtx) {
  // 成员变量获取sst读锁，构造完了解锁
  //实时的查询的时当时的数据，因此不能有实时的数据，即不能直接指向sst和memtable
  //获取他们的迭代器，获取当时的信息，heap是当时，contactor虽然是sst的指针但是，sst的信息是不会改变的
  //在compact前有效，在compact后无效
  // 1. 获取内存部分迭代器
  // TODO: 这里最好修改 memtable.begin 使其返回一个指针, 避免多余的内存拷贝
  if (engine_->memtable.get_total_size()<=0&&engine_->ssts.empty()) {
    return;
  }
  //memtable的迭代器
  iter_vec.emplace_back(engine_->memtable.begin(max_tranc_id));
  //levle0的迭代器
   std::vector<SstIterator> l0_sstT;
      for (auto& it : engine_->level_sst_ids[0]) {
          l0_sstT.emplace_back(engine_->ssts[it],max_tranc_id);
      }
     //keep——all——version不明确
      iter_vec.emplace_back(std::make_shared<HeapIterator>(std::move(SstIterator::merge_sst_iterator(std::move(l0_sstT),max_tranc_id,false).first)));
  //level1及以上的迭代器
     for (auto& lev_it:engine_->level_sst_ids) {
         if (lev_it.first==0) {
              continue;
         }
         std::vector<std::shared_ptr<SST>> sst_vec;
         for (auto& it : lev_it.second) {
             sst_vec.emplace_back(engine_->ssts[it]);
         }
         iter_vec.emplace_back(std::make_shared<ConcactIterator>(std::move(sst_vec),max_tranc_id,false));
     }
     //找到第一个有效key
     auto tmp=get_min_key_idx();
     cur_idx_=tmp.first;
      // if (engine_->memtable.get_total_size()<=0) {
      //       cur_idx_=1;
           //全空已经判断，一定有初始值
      //       while (iter_vec[cur_idx_]->is_end()) {
      //            ++cur_idx_;
      //       }
      // }
    update_current();
    rlock_.unlock();
}
//用来对多个层次的迭代器，分别选最小的
std::pair<size_t, std::string> Level_Iterator::get_min_key_idx() const {
    // if (is_end()) {
    //   return {-1, ""};
    // }
    //条件变量，wait，先抢锁
     auto iter_size=iter_vec.size();
    //  std::string min_key=(**iter_vec[cur_idx_]).first;
    //  size_t min_idx=0;
    //    for (size_t i=0; i<iter_size; ++i) {
    //        if (!iter_vec[i]->is_end()) {
    //            min_key=std::min(min_key,(**iter_vec[i]).first);
    //             if (min_key==(**iter_vec[i]).first) {
    //                 min_idx=i;
    //             }
    //        }
    //    }
    //保证第一次非空
     size_t min_idx = iter_vec.size();
    std::string min_key;

    for (size_t i = 0; i < iter_vec.size(); ++i) {
        if (iter_vec[i]->is_end()) continue;

        auto key = (**iter_vec[i]).first;
        if (min_idx == iter_vec.size() || key < min_key) {
            min_idx = i;
            min_key = key;
        }
    }
       return {min_idx, min_key};
}
//跳过同key
void Level_Iterator::skip_key(const std::string &key) {
  for (auto& it : iter_vec) {
        while (!it->is_end() && (**it).first == key) {
            ++(*it);
        }
    }
}

void Level_Iterator::update_current() const {
     if (is_end()) {
      cached_value=std::nullopt;
       return;
     }
    //cached_value=*(*iter_vec[cur_idx_]);
     auto raw = *(*iter_vec[cur_idx_]);
     raw.second = engine_->resolve_value_try(raw.second);
     cached_value = raw;
}

BaseIterator &Level_Iterator::operator++() {
    //   if (is_end()){
    //      return *this;
    //   }
    //   auto old_key = cached_value->first;
    //   skip_key(old_key);
    //  //tmp.second暂时不知道干什么
    //   iter_vec[cur_idx_]->operator++();
    //    auto tmp=get_min_key_idx();
    //  cur_idx_=tmp.first;
    //   if (iter_vec[cur_idx_]->is_end()) {
    //    auto iter_size=iter_vec.size();
    //    while (cur_idx_<iter_size&&iter_vec[cur_idx_]->is_end()) {
    //       ++cur_idx_;
    //    }
    //   }
    //   update_current();
    //   return  *this;
     if (is_end()) {
        return *this;
    }

    auto old_key = cached_value->first;
    skip_key(old_key);

    if (!is_end()) {
        cur_idx_ = get_min_key_idx().first;
    }

    update_current();
    return *this;
}
bool Level_Iterator::operator==(const BaseIterator &other) const {
      if (get_type()!=other.get_type()) {
         return false;
      }
      auto &other_iter=dynamic_cast<const Level_Iterator&>(other);
      if (is_end()&&other_iter.is_end()) {
          return true;  
      }
      if (is_end()||other.is_end()) {
          return false;
      }
      return (*this->cached_value)==(*other_iter.cached_value);
}

bool Level_Iterator::operator!=(const BaseIterator &other) const {
  return !(*this == other);
}

BaseIterator::value_type Level_Iterator::operator*() const {
     if (!cached_value) {
        throw std::out_of_range("Level_Iterator is at end");
     }
     return  cached_value.value();
}
BaseIterator::pointer Level_Iterator::operator->() const {
      if (!cached_value) {
          throw std::out_of_range("Level_Iterator is at end");
      }
      return &cached_value.value();
}
IteratorType Level_Iterator::get_type() const {
  return IteratorType::LevelIterator;
}

uint64_t Level_Iterator::get_tranc_id() const { return max_tranc_id_; }

bool Level_Iterator::is_end() const {
  for (auto &iter : iter_vec) {
    if ((*iter).is_valid()) {
      return false;
    }
  }
  return true;
}

bool Level_Iterator::is_valid() const { return !is_end(); }


} // namespace tiny_lsm
