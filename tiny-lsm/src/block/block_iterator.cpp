#include "block/block_iterator.h"
#include "block/block.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

class Block;

namespace tiny_lsm {
BlockIterator::BlockIterator(std::shared_ptr<Block> b, size_t index,
                             uint64_t tranc_id, bool keep_all_versions)
    : block(b), current_index(index), tranc_id_(tranc_id),
      cached_value(std::nullopt), keep_all_versions_(keep_all_versions) {
  skip_by_tranc_id();
}

BlockIterator::BlockIterator(std::shared_ptr<Block> b, const std::string &key,
                             uint64_t tranc_id, bool keep_all_versions)
    : block(b), tranc_id_(tranc_id), cached_value(std::nullopt),
      keep_all_versions_(keep_all_versions) {

      auto idx=block->get_idx_binary(key,tranc_id);
       if (idx) {
          current_index=idx.value();
       }else {
         current_index=block->offsets.size();
       }
      skip_by_tranc_id();
}

// BlockIterator::BlockIterator(std::shared_ptr<Block> b, uint64_t tranc_id)
//     : block(b), current_index(0), tranc_id_(tranc_id),
//       cached_value(std::nullopt) {
//   skip_by_tranc_id();
// }

BlockIterator::pointer BlockIterator::operator->() const {
      update_current();
      if (!cached_value) {
            return nullptr;
      }
      return  &cached_value.value();
}

BlockIterator &BlockIterator::operator++() {
     if (is_end()) {
            return *this;
     }
     std::string save_key;
     save_key=block->get_key_at(block->offsets[current_index]);
     ++current_index;
     if (is_end()) {
           return *this;
     }
     //去掉all_versions
     if (!keep_all_versions_) {
         while (!is_end()&&save_key==block->get_key_at(block->offsets[current_index])) {
               current_index++;
         }
     }
     //事务可见性
     skip_by_tranc_id();
     if (is_end()) {
           return *this;
     }
     //auto off=block->offsets[current_index];
    // tranc_id_=block->get_tranc_id_at(off);
     return  *this;
}

bool BlockIterator::operator==(const BlockIterator &other) const {

        if ((!block||current_index>=block->offsets.size())&&
              (!other.block||other.current_index>=other.block->offsets.size())) {
                 return  true;
              }
        if ((!block||current_index>=block->offsets.size())
                  || (!other.block||other.current_index>=other.block->offsets.size())) {
                return false;
        }
        return block==other.block&&current_index==other.current_index;
}

bool BlockIterator::operator!=(const BlockIterator &other) const {
      return  !(operator==(other));
}

BlockIterator::value_type BlockIterator::operator*() const {
     update_current();
     if (!cached_value) {
         return std::make_pair("","");
     }
     return  cached_value.value();
}

bool BlockIterator::is_end() { return !block||current_index >= block->offsets.size(); }

uint64_t BlockIterator::get_cur_tranc_id() const {
  if (!block || current_index >= block->offsets.size()) {
    return 0;
  }
  size_t offset = block->get_offset_at(current_index);
  return block->get_tranc_id_at(offset);
}

void BlockIterator::update_current() const {
      if (!block || current_index >= block->offsets.size()) {
             cached_value=std::nullopt;
             return;
      }
      auto idx=block->offsets[current_index];
      auto key=block->get_key_at(idx);
      auto value=block->get_value_at(idx);
      cached_value=std::make_pair(key,value);
}

void BlockIterator::skip_by_tranc_id() {
  if (tranc_id_==0) {
     return;
  }
  while (get_cur_tranc_id()>tranc_id_) {
       ++current_index;
  }

} // namespace tiny_lsm
}
