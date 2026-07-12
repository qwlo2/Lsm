#include "iterator/iterator.h"
//#include "gmock/gmock.h"
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace tiny_lsm {

// *************************** SearchItem ***************************
bool operator<(const SearchItem &a, const SearchItem &b) {
  if (a.key_ != b.key_){
     return a.key_ < b.key_;
  }
  if (a.level_ != b.level_){
    return a.level_ < b.level_;
  }
  if (a.idx_ != b.idx_){
    return a.idx_ < b.idx_;
  }
  return a.tranc_id_ > b.tranc_id_;
}

bool operator>(const SearchItem &a, const SearchItem &b) {
    if (a.key_ != b.key_){
     return a.key_ > b.key_;
  }
  if (a.level_ != b.level_){
    return a.level_ > b.level_;
  }
  if (a.idx_ != b.idx_){
    return a.idx_ > b.idx_;
  }
  return a.tranc_id_ < b.tranc_id_;
}

bool operator==(const SearchItem &a, const SearchItem &b) {
  return a.idx_ == b.idx_ && a.key_ == b.key_&&a.tranc_id_ == b.tranc_id_&&a.level_ == b.level_;
}

// *************************** HeapIterator ***************************
HeapIterator::HeapIterator(bool skip_delete, bool keep_all_versions)
    : skip_delete_(skip_delete), keep_all_versions_(keep_all_versions) {
  // 默认构造函数
}
//初始化只到第一个有效数字，方便++放到与谓词查询一起
HeapIterator::HeapIterator(std::vector<SearchItem> item_vec,
                           uint64_t max_tranc_id, bool skip_delete,
                           bool keep_all_versions)
    : max_tranc_id_(max_tranc_id), skip_delete_(skip_delete),
      keep_all_versions_(keep_all_versions) , items(std::greater<SearchItem>(), std::move(item_vec)){
        if (!items.empty()&&!top_value_legal()) {
              operator++();
        }
  //初始化时进行初步的过滤
}
//top_value_legal 用来判断当前堆顶元素是否合法,合法则->之直接使用，封装update_current
HeapIterator::pointer HeapIterator::operator->() const { 
      update_current();
      return  current.get();
}

HeapIterator::value_type HeapIterator::operator*() const {
      update_current();
      if (current) {
       return std::make_pair(current->first, current->second);
      }
     return  std::make_pair("", ""); // 或者抛出异常
}
//跳到下一个有用的key，要分为导出与刷盘
//是HeapIterator的自增,并没有时刻更新curren，
//skip_delete_=true则删除记录一定弹出，keep_all_versions_只保护非删除记录的版本，删除记录不受保护
BaseIterator &HeapIterator::operator++() {
         auto top_item = items.top();
         items.pop();
         if (items.empty()) {
            return  *this;
        }
        //kepp_all_versions_为true时，保留同一key的所有版本
        if (!keep_all_versions_) {
           while (!items.empty() && items.top().key_==top_item.key_) {
            items.pop();
           }
        }
       //在新的key先过滤可见性，delete不一定可见
       skip_by_tranc_id();
       //判断skip-delete，keep，已经可见性
           while(!items.empty() && !top_value_legal()) {
                    if (!keep_all_versions_) {
                      //keep_all_versions_为true时，保留同一key的所有版本，删除记录不受保护
                       auto del_item = items.top();
                          while (!items.empty() && del_item.key_==items.top().key_) {
                            items.pop();
                          }
                    } else {
                        //skip_delete_==true,keep_all_versions_==true
                        //只弹出删除记录
                         if (!items.empty() && items.top().value_.empty()) {
                                 items.pop();
                          }   
                    }
              //keep_all_versions_为flase时，则重复过滤，判断k和s这2个
              //true时，pop后若key不变则一定可见，否则重复过滤，判断k和s这2个
               skip_by_tranc_id();
        }
        //一个heap_iterator就是一个可遍历的memtable或者一个sstable的迭代器
        return *this;
}
//仅比较top,对于迭代器而言只需比较它指向的cureent
bool HeapIterator::operator==(const BaseIterator &other) const {
           if (get_type()==other.get_type()) {
              auto &other_heap = static_cast<const HeapIterator&>(other);
               if (items.empty() && other_heap.items.empty()) {
                    return true;
                }      
               else if (items.empty() || other_heap.items.empty()) {
                      return false;
                  }
               else {
                      return  items.top()==other_heap.items.top();
                  }           
           }
           return  false;
}

bool HeapIterator::operator!=(const BaseIterator &other) const {
     return  !(*this == other);
}

bool HeapIterator::top_value_legal() const {
        if (items.empty()||skip_delete_&&items.top().value_.empty()) {
           return false;
        }
      return true;
}
void HeapIterator::skip_by_tranc_id() {
    if (max_tranc_id_==0) {
       return;
    }
    while (!items.empty()&&items.top().tranc_id_>max_tranc_id_) {
            items.pop();
    }
}

bool HeapIterator::is_end() const { return items.empty(); }
bool HeapIterator::is_valid() const { return !items.empty(); }
//用来获取当前堆顶元素的key和value，更新current指针
void HeapIterator::update_current() const {
        if (!items.empty()) {
            current = std::make_shared<std::pair<std::string,std::string>>(items.top().key_, items.top().value_);
        } else {
            current.reset();  
        }
}

IteratorType HeapIterator::get_type() const {
  return IteratorType::HeapIterator;
}

uint64_t HeapIterator::get_tranc_id() const {
  if (keep_all_versions_ && !items.empty()) {
    return items.top().tranc_id_;
  }
  return max_tranc_id_;
}
} // namespace tiny_lsm