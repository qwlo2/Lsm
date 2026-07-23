#include "lsm/two_merge_iterator.h"
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

namespace tiny_lsm {

TwoMergeIterator::TwoMergeIterator() {}

TwoMergeIterator::TwoMergeIterator(std::shared_ptr<BaseIterator> it_a,
                                   std::shared_ptr<BaseIterator> it_b,
                                   uint64_t max_tranc_id,
                                   bool keep_all_versions)
    : it_a(std::move(it_a)), it_b(std::move(it_b)),
      max_tranc_id_(max_tranc_id), keep_all_versions_(keep_all_versions) {
  // 先跳过不可见的事务
  skip_by_tranc_id();
  skip_it_b();              // 跳过与 it_a 重复的 key
  choose_a = choose_it_a(); 
  //正常查询的合并与sst的合并，但都要进行比较，在update中进行，++时choose——a代表是a还是b已经pop
  //对于true，此时同key下，a更新，因此优先级更高，b跳过
}
//对比较的封装
bool TwoMergeIterator::choose_it_a() {
   if (!it_a||it_a->is_end()) {
    return false;
   }
  //  if (it_a->get_type()==IteratorType::MemTableIterator) {
  //   return true;
  //  }
   if (!it_b||it_b->is_end()) {
    return true; 
   }
   if ((**it_a).first < (**it_b).first) {
        return true;
   }
   if ((**it_a).first == (**it_b).first&&
         it_a->get_tranc_id()>= it_b->get_tranc_id() ) {
        return true;
   }
   return false;
}
//去重，所有迭代器都有keep——all——versions，异常a，b若不kepp则只有一个版本，只需跳一次
void TwoMergeIterator::skip_it_b() {
  if (keep_all_versions_) {
    return;
  }
  if (!it_a || !it_b) {
    return;
  }
  if (!it_a->is_end() && !it_b->is_end() && (**it_a).first == (**it_b).first) {
    ++(*it_b);
  }
}

void TwoMergeIterator::skip_by_tranc_id() {
  
}
//sst和blokc的迭代器由于要肩负快照读的责任，concact要对lsm的迭代器做++，所以它们的实现
//跳到第一个满足条件的key并掠过同key的是对的
//合并时的挑选在concact，txn-id等于0让sst返回所有
//所有>active-oldest-txnid的保留
//第一个<=也保留，预防只有=或小于active-oldest-txnid的key的情况
//
BaseIterator &TwoMergeIterator::operator++() {
       if (is_end()) {
        return *this;
       }
       if (choose_a) {
        ++(*it_a);
       } else {
        ++(*it_b);
       }
       //在pop后，根据choose++,再比较跳过，最后更新choose
       choose_a = choose_it_a();
      skip_it_b();
         //skip_by_tranc_id();
       return *this;
}

bool TwoMergeIterator::operator==(const BaseIterator &other) const {
  if (other.get_type() != IteratorType::TwoMergeIterator) {
    return false;
  }
  const TwoMergeIterator &other_it = static_cast<const TwoMergeIterator &>(other);
  if (is_end() && other_it.is_end()) {
    return true;
  }
  if (is_end() || other_it.is_end()) {
    return false;
  }
  return (**this) == (*other_it);
}

bool TwoMergeIterator::operator!=(const BaseIterator &other) const {
    return !(*this == other);
}

BaseIterator::value_type TwoMergeIterator::operator*() const {
      update_current();
      if (current) {
        return *current;
      }
      throw std::out_of_range("TwoMergeIterator: no more elements");
}
TwoMergeIterator::pointer TwoMergeIterator::operator->() const {
  update_current();
  if (current) {
    return current.get();
  }
  throw std::out_of_range("TwoMergeIterator: no more elements");
}
IteratorType TwoMergeIterator::get_type() const {
  return IteratorType::TwoMergeIterator;
}

uint64_t TwoMergeIterator::get_tranc_id() const {
  if (keep_all_versions_) {
    if (choose_a && it_a && it_a->is_valid()) {
      return it_a->get_tranc_id();
    }
    if (!choose_a && it_b && it_b->is_valid()) {
      return it_b->get_tranc_id();
    }
  }
  return max_tranc_id_;
}

bool TwoMergeIterator::is_end() const {
  if (it_a == nullptr && it_b == nullptr) {
    return true;
  }
  if (it_a == nullptr) {
    return it_b->is_end();
  }
  if (it_b == nullptr) {
    return it_a->is_end();
  }
  return it_a->is_end() && it_b->is_end();
}

bool TwoMergeIterator::is_valid() const {
  if (it_a == nullptr && it_b == nullptr) {
    return false;
  }
  if (it_a == nullptr) {
    return it_b->is_valid();
  }
  if (it_b == nullptr) {
    return it_a->is_valid();
  }
  return it_a->is_valid() || it_b->is_valid();
}

void TwoMergeIterator::update_current() const {
      if (is_end()) {
        current = nullptr;
        return;
      }
      //在++逻辑中确认谁是下一个，choose代表了归并中，下一个选谁
      if (choose_a) {
        current = std::make_shared<value_type>(**it_a);
      } else {
        current = std::make_shared<value_type>(**it_b);
      }
     return;
}
} // namespace tiny_lsm
