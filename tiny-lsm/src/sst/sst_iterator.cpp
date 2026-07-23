#include "sst/sst_iterator.h"
#include "block/block.h"
#include "block/block_iterator.h"
#include "iterator/iterator.h"
#include "sst/sst.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace tiny_lsm {

// predicate返回值:
//   0: 谓词
//   >0: 不满足谓词, 需要向右移动
//   <0: 不满足谓词, 需要向左移动
std::optional<std::pair<SstIterator, SstIterator>> sst_iters_monotony_predicate(
    std::shared_ptr<SST> sst, uint64_t tranc_id,
    std::function<int(const std::string &)> predicate) {
    int left=0,right=sst->num_blocks()-1;
    int mid;
   //找到【first，end】
     while (left<=right) {
         mid=left+(right-left)/2;
         int l_key=predicate(sst->meta_entries[mid].last_key);
          //f_key<=0和f_key>=0,l_key<=0
          if (l_key>0) {
               left=mid+1;
            }      
          else {
               right=mid-1;
            }
          }
          int first=left;
          if (first>=sst->num_blocks()) {
               return std::nullopt;
          }
          left=0,right=sst->num_blocks()-1;
          while (left<=right) {
               mid=left+(right-left)/2;
         int f_key=predicate(sst->meta_entries[mid].first_key);
          if (f_key<0) {
              right=mid-1;
            }      
          else {
               left=mid+1;
            }
          }
          int last=right;
          //等于时当只有一个block也成立
       if (first>last) {
            return std::nullopt;
       } 
       //first——block
         auto  f_block= sst->read_block(first);
       auto tmp=f_block->get_monotony_predicate_iters(tranc_id,predicate);
       if (!tmp) {
          return std::nullopt;
       }
    
       auto f_block_sst=SstIterator(tranc_id);
        //auto f_block_sst=SstIterator(tranc_id);
        f_block_sst.set_m_sst(sst);
       f_block_sst.set_block_idx(first);
        f_block_sst.set_block_it( tmp->first);
       //last——block
       
        auto  l_block= sst->read_block(last);
          tmp=l_block->get_monotony_predicate_iters(tranc_id,predicate);
       if (!tmp) {
          return std::nullopt;
       }
   
        auto l_block_sst=SstIterator(tranc_id);
        l_block_sst.set_m_sst(sst);
       l_block_sst.set_block_idx(last);
        l_block_sst.set_block_it(tmp->second);

        //f_block_sst.skip_by_tranc_id();
       //l_block_sst.skip_by_tranc_id();
       //end（）时，若txn==0，会导致++跳过end，
       //skip中只处理非0，非end的
       if (l_block_sst.m_block_it->is_end()) {
          ++l_block_sst;
        }
        if (f_block_sst.m_block_it->is_end()) {
            ++f_block_sst;
        }
        //f和l都刚好end
        // if (f_block_sst.m_block_it->is_end()) {
        //     ++f_block_sst;
        // }
        
        return std::make_pair(f_block_sst,l_block_sst);
  }
SstIterator::SstIterator(std::shared_ptr<SST> sst, uint64_t tranc_id,
                         bool keep_all_versions)
    : m_sst(sst), m_block_idx(0), m_block_it(nullptr), max_tranc_id_(tranc_id),
      keep_all_versions_(keep_all_versions) {
  if (m_sst) {
    seek_first();
   // skip_by_tranc_id();
   if (m_block_it&&m_block_it->is_end()) {
        operator++();
   }
  }
}

SstIterator::SstIterator(std::shared_ptr<SST> sst, const std::string &key,
                         uint64_t tranc_id, bool keep_all_versions)
    : m_sst(sst), m_block_idx(0), m_block_it(nullptr), max_tranc_id_(tranc_id),
      keep_all_versions_(keep_all_versions) {
  if (m_sst) {
    seek(key);
    //skip_by_tranc_id();
    if (m_block_it&&m_block_it->is_end()) {
        operator++();
   }
  }
}

void SstIterator::set_block_idx(size_t idx) { m_block_idx = idx; }
void SstIterator::set_block_it(std::shared_ptr<BlockIterator> it) {
  m_block_it = it;
}

void SstIterator::seek_first() {
     seek(m_sst->get_first_key());
}

void SstIterator::seek(const std::string &key) {
     cached_value=std::nullopt;
     auto block_idx=m_sst->find_block_idx(key);
     if (block_idx==-1) {
          m_block_idx = m_sst->num_blocks();
         // m_block_it = nullptr;
         //cached_value = std::nullopt;
         return;
     }
     auto block_data=m_sst->read_block(block_idx);
     m_block_idx=block_idx;
     auto tmp=block_data->get_idx_binary(key,max_tranc_id_);
     if (!tmp) {
       m_block_idx=m_sst->num_blocks();
      // m_block_it = nullptr;
          return;
     }

     m_block_it=std::make_shared<BlockIterator>(block_data,tmp.value(),max_tranc_id_,keep_all_versions_);
     if (m_block_it->is_end()) {
        operator++();
     }
     auto raw=**m_block_it;
   
    // raw.second=m_sst->resolve_value(raw.second);
     cached_value=raw;
}

std::string SstIterator::key() {
  if (!m_block_it||m_block_it->is_end()||
    m_block_idx >= static_cast<int64_t>(m_sst->num_blocks())) {
    throw std::runtime_error("Iterator is invalid");
  }
  return (*m_block_it)->first;
}

std::string SstIterator::value() {
  if (!m_block_it||m_block_it->is_end()||
    m_block_idx >= static_cast<int64_t>(m_sst->num_blocks())) {
    throw std::runtime_error("Iterator is invalid");
  }
 //return m_sst->resolve_value((*m_block_it)->second);
   return (*m_block_it)->second;
}

BaseIterator &SstIterator::operator++() {
     if (!m_sst||!m_block_it) {
         return *this;
     }
     ++(*m_block_it);

     while(m_block_it->is_end()) {
         ++m_block_idx;
          if (m_block_idx>=m_sst->num_blocks()) {
                 return *this;
          }
          //新的block-it
       auto tmp=m_sst->read_block(m_block_idx);
       //构造函数会skip-txn
       m_block_it=std::make_shared<BlockIterator>(tmp,0,max_tranc_id_,keep_all_versions_);
     }
     return *this;
}
//
void SstIterator::skip_by_tranc_id() {
  if (max_tranc_id_==0) {
       return;
  }

  if (!m_block_it->is_end()&&get_cur_tranc_id()>max_tranc_id_) {
           ++(*m_block_it);
     while(m_block_it->is_end()) {
         ++m_block_idx;
          if (m_block_idx>=m_sst->num_blocks()) {
                 return ;
          }
          //新的block-it
       auto tmp=m_sst->read_block(m_block_idx);
       //构造函数会skip-txn
       m_block_it=std::make_shared<BlockIterator>(tmp,0,max_tranc_id_);
     }
  }

} 
bool SstIterator::operator==(const BaseIterator &other) const {
  if (other.get_type() != IteratorType::SstIterator) {
    return false;
  }
  const auto &other_it = static_cast<const SstIterator &>(other);
  if (is_end() && other_it.is_end()) {
    return true;
  }
  if (is_end() || other_it.is_end()) {
    return false;
  }
  return m_sst == other_it.m_sst &&
         m_block_idx == other_it.m_block_idx &&
         *m_block_it == *other_it.m_block_it;
}

bool SstIterator::operator!=(const BaseIterator &other) const {
    return !operator==(other);
}

SstIterator::value_type SstIterator::operator*() const {
  if (!m_sst||!m_block_it||m_block_it->is_end()||m_block_idx>=m_sst->num_blocks()) {
       return std::make_pair("", "");
    }

   update_current();
   if (!cached_value) {
      return std::make_pair("", "");
   }
  return cached_value.value();
}

IteratorType SstIterator::get_type() const { return IteratorType::SstIterator; }

uint64_t SstIterator::get_tranc_id() const {
  if (keep_all_versions_ && m_block_it) {
    return m_block_it->get_cur_tranc_id();
  }
  return max_tranc_id_;
}
bool SstIterator::is_end() const { 
       return !m_sst || !m_block_it ||
         m_block_idx >= static_cast<int64_t>(m_sst->num_blocks()); 
  }

bool SstIterator::is_valid() const {
  return m_sst&&m_block_it && !m_block_it->is_end() &&
         m_block_idx < m_sst->num_blocks();
}
SstIterator::pointer SstIterator::operator->() const {
     if (!m_sst||!m_block_it||m_block_it->is_end()||m_block_idx>=m_sst->num_blocks()) {
       return nullptr;
    }
   update_current();
   if (!cached_value) {
       return nullptr;
   }
   return   &cached_value.value();
}

void SstIterator::update_current() const {
  if ( m_block_it && !m_block_it->is_end()) {
    auto raw = *(*m_block_it);
    //在wisky下，value是地址，resolve_value进行一次转换
    //raw.second = m_sst->resolve_value(raw.second);
    cached_value = raw;
  }
}

uint64_t SstIterator::get_cur_tranc_id() const {
  if (!m_block_it) {
    return 0;
  }
  return m_block_it->get_cur_tranc_id();
}
//只能用于level0
std::pair<HeapIterator, HeapIterator>
SstIterator::merge_sst_iterator(std::vector<SstIterator> iter_vec,
                                uint64_t tranc_id, bool keep_all_versions) {
  if (iter_vec.empty()) {
    return std::make_pair(HeapIterator(), HeapIterator());
  }

  HeapIterator it_begin(false, keep_all_versions); // 不跳过删除元素
 for (auto &iter : iter_vec) {
    while (iter.is_valid() && !iter.is_end()) {
      // it_begin.items.emplace(
      //     iter.key(), iter.m_sst->resolve_value(iter.m_block_it->operator*().second),
      //     -iter.m_sst->get_sst_id(), 0,
      //     iter.get_cur_tranc_id()); // ! 此处的level暂时没有作用, 都作用于同一层的比较
      // ++iter;
       it_begin.items.emplace(
          iter.key(), iter.m_block_it->operator*().second,
          -iter.m_sst->get_sst_id(), 0,
          iter.get_cur_tranc_id()); // ! 此处的level暂时没有作用, 都作用于同一层的比较
      ++iter;
    }
  }
  return std::make_pair(it_begin, HeapIterator());
} 
} // namespace tiny_lsm
