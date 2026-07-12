#include "sst/concact_iterator.h"
#include <stdexcept>

namespace tiny_lsm {

ConcactIterator::ConcactIterator(std::vector<std::shared_ptr<SST>> ssts,
                                 uint64_t tranc_id, bool keep_all_versions)
    : ssts(ssts), cur_iter(nullptr,tranc_id), cur_idx(0),
      max_tranc_id_(tranc_id), keep_all_versions_(keep_all_versions) {
        if (!ssts.empty()) {
            //max_tranc_id_=ssts[cur_idx]->get_tranc_id_range().second;
            cur_iter=ssts[cur_idx]->begin(tranc_id, keep_all_versions);
        }
}
BaseIterator &ConcactIterator::operator++() {
  ++cur_iter;
  if (cur_iter.is_end()) {
    ++cur_idx;
    if (cur_idx < ssts.size()) {
      //max_tranc_id_ = ssts[cur_idx]->get_tranc_id_range().second;
      cur_iter = ssts[cur_idx]->begin(max_tranc_id_, keep_all_versions_);
    }
  }
  return *this;
}    

bool ConcactIterator::operator==(const BaseIterator &other) const {
         if (get_type()==other.get_type()) {
              auto other_concact_iter=static_cast<const ConcactIterator&>(other);
              if (is_end() && other_concact_iter.is_end()) {
                    return true;
              }
              if (is_end() || other_concact_iter.is_end()) {
                    return false;
              }
              if (cur_iter == other_concact_iter.cur_iter && cur_idx == other_concact_iter.cur_idx) {
                    return true;
              }
         }
         return false;
}

bool ConcactIterator::operator!=(const BaseIterator &other) const {
  return !(*this == other);
}

ConcactIterator::value_type ConcactIterator::operator*() const {
  if (!is_valid()) {
    throw std::out_of_range("ConcactIterator::operator*: invalid iterator");
  }
  return *cur_iter;
}

IteratorType ConcactIterator::get_type() const {
  return IteratorType::ConcactIterator;
}

uint64_t ConcactIterator::get_tranc_id() const {
  if (keep_all_versions_) {
    return cur_iter.get_tranc_id();
  }
  return max_tranc_id_;
}

bool ConcactIterator::is_end() const {
         return  cur_idx >= ssts.size() || !cur_iter.is_valid();
}

bool ConcactIterator::is_valid() const {
     return cur_idx < ssts.size() && cur_iter.is_valid();
}
//越界用out_of_range异常,标准库对*，->操作符的迭代器越界访问不做检查
ConcactIterator::pointer ConcactIterator::operator->() const {
  if (!is_valid()) {
    throw std::out_of_range("ConcactIterator::operator->: invalid iterator");
  }
  return cur_iter.operator->();
}

std::string ConcactIterator::key() { return cur_iter.key(); }

std::string ConcactIterator::value() { return cur_iter.value(); }
} // namespace tiny_lsm
