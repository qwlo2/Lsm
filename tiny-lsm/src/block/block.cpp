#include "block/block.h"
#include "block/block_iterator.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
namespace tiny_lsm {
Block::Block(size_t capacity) : capacity(capacity) {}

std::vector<uint8_t> Block::encode(bool with_hash) {
  // TODO: Lab 3.1 编码单个类实例形成一段字节数组
  // ? 格式: [data段] + [offsets数组, 每项uint16_t] + [元素个数 uint16_t]
  // ? 若 with_hash == true, 末尾额外追加 uint32_t 的 CRC 校验值
  // ? CRC 覆盖除自身之外的所有字节
  std::vector<uint8_t> ans;
  // std::make_move_iterator(it) 是让迭代器解引用出来的元素变成右值。
  ans.insert(ans.end(), data.begin(), data.end());
  auto uint16_t_push = [&ans](uint16_t &x) {
    // x&0xff,即0x00ff，只保留了低8位，再转为uint8-t
    ans.emplace_back(static_cast<uint8_t>(x & 0xff));
    ans.emplace_back(static_cast<uint8_t>((x >> 8) & 0xff));
  };
  for (auto &it : offsets) {
    uint16_t_push(it);
  }
  uint16_t entry_size = static_cast<uint16_t>(offsets.size());
  uint16_t_push(entry_size);
  auto crc32 = [](const std::vector<uint8_t> &data) {
    uint32_t crc = 0xffffffff;
    for (auto &it : data) {
      crc ^= it;
      for (int i = 0; i < 8; i++) {
        if (crc & 1) {
          crc = (crc >> 1) ^ 0xEDB88320;
        } else {
          crc >>= 1;
        }
      }
    }
    return crc ^ 0xffffffff;
  };
  // 一位位^的结果和下一字节相加（^），得到的结果与crc原理一样
  // 赋值0xffffffff，最后^，是标准化的改动
  // 0xEDB88320是反向（低->高），正向为它的相反，0x04C11DB7
  if (with_hash) {
    auto crc = crc32(ans);
    for (int i = 0; i < 4; i++) {
      ans.emplace_back(static_cast<uint8_t>((crc >> (i * 8)) & 0xff));
    }
  }
  return ans;
}

std::shared_ptr<Block> Block::decode(const std::vector<uint8_t> &encoded,
                                     bool with_hash) {
  int size = encoded.size(), offset_beg;
  uint16_t entry_size;
  if (size < 2) {
    throw std::runtime_error("encoded size<2");
  }
  if (with_hash) {
    if (size < 6) {
      throw std::runtime_error("crc 校验错误(size<6)");
    }
    uint32_t save;
    memcpy(&save, encoded.data() + size - 4, sizeof(uint32_t));
    memcpy(&entry_size, encoded.data() + size - 6, sizeof(uint16_t));
    offset_beg = size - 6 - 2 * entry_size;
    if (offset_beg < 0) {
      throw std::runtime_error("invalid entry_size");
    }
    auto crc32 = [](const std::vector<uint8_t> &data, unsigned int size) {
      uint32_t crc = 0xffffffff;
      for (int i = 0; i < size; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
          if (crc & 1) {
            crc = (crc >> 1) ^ 0xEDB88320;
          } else {
            crc >>= 1;
          }
        }
      }
      return crc ^ 0xffffffff;
    };
    auto crc = crc32(encoded, size - 4);
    if (save != crc) {
      throw std::runtime_error("crc 校验错误");
    }
  }
  if (!with_hash) {
    memcpy(&entry_size, encoded.data() + size - 2, sizeof(uint16_t));
    offset_beg = size - 2 - 2 * entry_size;
    if (offset_beg < 0) {
      throw std::runtime_error("invalid entry_size");
    }
  }
   auto ans = std::make_shared<Block>(entry_size);
  ans->data.insert(ans->data.begin(), encoded.begin(),
                   encoded.begin() + offset_beg);
  auto read_u16=[](const std::vector<uint8_t>& data ,int pos){
           return static_cast<uint16_t>(data[pos])|
                  static_cast<uint16_t>(data[pos+1]<<8);
  };
  for (int i=0;i<entry_size;++i) {
        auto entry=read_u16(encoded,offset_beg+i*sizeof(uint16_t));
        ans->offsets.emplace_back(entry);
  }
  return ans;
}

std::string Block::get_first_key() {
  if (data.empty() || offsets.empty()) {
    return "";
  }

  // 读取第一个key的长度（前2字节）
  uint16_t key_len;
  memcpy(&key_len, data.data(), sizeof(uint16_t));

  // 读取key
  std::string key(reinterpret_cast<char *>(data.data() + sizeof(uint16_t)),
                  key_len);
  return key;
}

size_t Block::get_offset_at(size_t idx) const {
  if (idx > offsets.size()) {
    throw std::runtime_error("idx out of offsets range");
  }
  return offsets[idx];
}
// TODO: Lab 3.1 添加一个键值对到block中
// ? 每条 entry 格式:
// [key_len:uint16_t][key][value_len:uint16_t][value][tranc_id:uint64_t] ? 若
// !force_write 且当前容量不足则返回 false ? 成功添加后记录偏移到 offsets, 返回
// true
bool Block::add_entry(const std::string &key, const std::string &value,
                      uint64_t tranc_id, bool force_write) {
  if (cur_size() >= capacity && !force_write) {
    return false;
  }
  auto ofs = data.size();
  offsets.emplace_back(ofs);

  uint16_t key_len = static_cast<uint16_t>(key.size());
  uint16_t value_len = static_cast<uint16_t>(value.size());
  auto uint16_t_push = [this](uint16_t &x) {
    data.emplace_back(static_cast<uint8_t>(x & 0xff));
    data.emplace_back(static_cast<uint8_t>((x >> 8) & 0xff));
  };
  uint16_t_push(key_len);
  data.insert(data.end(), key.begin(), key.end());
  uint16_t_push(value_len);
  data.insert(data.end(), value.begin(), value.end());
  for (int i = 0; i < 8; i++) {
    data.emplace_back(static_cast<uint8_t>((tranc_id >> (i * 8)) & 0xff));
  }
  return true;
}

// 从指定偏移量获取entry的key
std::string Block::get_key_at(size_t offset) const {
  // TODO: Lab 3.1 从指定偏移量获取entry的key
  // ? 读取 data[offset] 处的 uint16_t key_len, 再取后续 key_len 个字节
  uint16_t key_len;
  memcpy(&key_len, data.data() + offset, sizeof(uint16_t));
  std::string key(
      reinterpret_cast<const char *>(data.data() + offset + sizeof(uint16_t)),
      key_len);
  return key;
}

// 从指定偏移量获取entry的value
// TODO: Lab 3.1 从指定偏移量获取entry的value
// ? 先跳过 key_len + key, 再读取 uint16_t value_len, 最后取 value
std::string Block::get_value_at(size_t offset) const {
  uint16_t key_len;
  memcpy(&key_len, data.data() + offset, sizeof(uint16_t));
  int skip_size = offset + key_len + sizeof(uint16_t);
  uint16_t value_len;
  memcpy(&value_len, data.data() + skip_size, sizeof(uint16_t));
  skip_size += sizeof(uint16_t);
  std::string value(reinterpret_cast<const char *>(data.data() + skip_size),
                    value_len);
  return value;
}
// TODO: Lab 3.1 从指定偏移量获取entry的tranc_id
// ? 先跳过 key 和 value, 读取末尾的 uint64_t tranc_id
uint64_t Block::get_tranc_id_at(size_t offset) const {
  int skip_size = offset;
  uint16_t key_len;
  memcpy(&key_len, data.data() + skip_size, sizeof(uint16_t));
  skip_size += key_len + sizeof(uint16_t);
  uint16_t value_len;
  memcpy(&value_len, data.data() + skip_size, sizeof(uint16_t));
  skip_size += sizeof(uint16_t) + value_len;
  uint64_t txn;
  memcpy(&txn, data.data() + skip_size, sizeof(uint64_t));
  return txn;
}

// 比较指定偏移量处的key与目标key
int Block::compare_key_at(size_t offset, const std::string &target) const {
  std::string key = get_key_at(offset);
  return key.compare(target);
}

// 相同的key连续分布, 且相同的key的事务id从大到小排布
// 这里的逻辑是找到最接近 tranc_id 的键值对的索引位置
int Block::adjust_idx_by_tranc_id(size_t idx, uint64_t tranc_id) {
  int size = offsets.size();
  std::string target = get_key_at(offsets[idx]);
  int tmp = idx;
    while (tmp > 0 &&is_same_key(tmp-1,target)) {
      --tmp;
    }
  if (tranc_id == 0) {
    return tmp;
  } else {
    //从idx开始向后查找, 直到找到第一个tranc_id <= tranc_id的entry
    for (int i = tmp; i < size && is_same_key(i,target); ++i) {
      auto cur_txn = get_tranc_id_at(offsets[i]);
      if (cur_txn <= tranc_id) {
        return i;
      }
    }
    //target没有符合条件的
    return -1;
  }
}

bool Block::is_same_key(size_t idx, const std::string &target_key) const {
  if (idx >= offsets.size()) {
    return false; // 索引超出范围
  }
  return get_key_at(offsets[idx]) == target_key;
}

// 使用二分查找获取value
// 要求在插入数据时有序插入
std::optional<std::string> Block::get_value_binary(const std::string &key,
                                                   uint64_t tranc_id) {
  auto idx = get_idx_binary(key, tranc_id);
  if (!idx.has_value()) {
    return std::nullopt;
  }

  return get_value_at(offsets[*idx]);
}

std::optional<size_t> Block::get_idx_binary(const std::string &key,
                                            uint64_t tranc_id) {
  if (offsets.empty()) {
    return std::nullopt;
  }
  int left = 0, right = offsets.size() - 1;
  while (left <= right) {
    int mid = left + (right - left) / 2;
    int cnt = compare_key_at(offsets[mid], key);
    if (cnt > 0) {
      right = mid - 1;
    } else if (cnt < 0) {
      left = mid + 1;
    } else {
      int adjusted = adjust_idx_by_tranc_id(mid, tranc_id);
      if (adjusted == -1) {
          return std::nullopt;
      }
      return adjusted;
    }
  }
  return std::nullopt;
}

std::optional<
    std::pair<std::shared_ptr<BlockIterator>, std::shared_ptr<BlockIterator>>>
Block::iters_preffix(uint64_t tranc_id, const std::string &preffix) {
  // TODO: Lab 3.3 获取前缀匹配的区间迭代器
  // ? 将前缀匹配转化为单调谓词, 调用 get_monotony_predicate_iters
  // ? 谓词: -key.compare(0, preffix.size(), preffix)
  return get_monotony_predicate_iters(tranc_id,[this,&preffix](const std::string& s)->int{
          int ans=s.compare(0,preffix.size(),preffix);
           if (ans>0) {
              return  -1;
           } 
           if (ans<0) {
               return 1;
           } 
             return 0;
  });  
}

// 返回第一个满足谓词的位置和最后一个满足谓词的位置
// 如果不存在, 返回nullopt
// 谓词作用于key, 且保证满足谓词的结果只在一段连续的区间内, 例如前缀匹配的谓词
// 返回的区间是闭区间, 开区间需要手动对返回值自增
// predicate返回值:
//   0: 满足谓词
//   >0: 不满足谓词, 需要向右移动
//   <0: 不满足谓词, 需要向左移动
//txn的事务可见性应该如memtable的迭代器一样，在迭代器的++中实现
std::optional<
    std::pair<std::shared_ptr<BlockIterator>, std::shared_ptr<BlockIterator>>>
Block::get_monotony_predicate_iters(
    uint64_t tranc_id, std::function<int(const std::string &)> predicate) {
  // TODO: Lab 3.3 使用二分查找获取满足谓词的区间迭代器
  // ? 第一次二分: 找到 first (满足谓词的最左边索引)
  // ? 第二次二分: 找到 last  (满足谓词的最右边索引)
  // ? 返回 [BlockIterator(first), BlockIterator(last+1)]
  if (offsets.empty()) {
    return std::nullopt;
  }
  int left=0,right=offsets.size()-1;
  int mid=0,first=-1,last=-1;
  while (left<=right) {
    mid=left+(right-left)/2;
      int cnt=predicate(get_key_at(offsets[mid]));
      if (cnt>0) {
         left=mid+1;
      }
      else {
       if (cnt == 0) {
        first = mid;
      }//left与right最终会重合在target前，但当rarget为head时，必须要left<=right
      //这2种情况的first，分别为left+1和left，因此用first记录最后的target
      right = mid - 1;
      }
  }
  //不用考虑特殊情况，为-1即从未出现
   if (first==-1) {
       return std::nullopt;
   }
  left=0;right=offsets.size()-1;
  while (left<=right) {
      mid=left+(right-left)/2;
      int cnt=predicate(get_key_at(offsets[mid]));
      if (cnt<0) {
         right=mid-1;
      }
      else {
        if (cnt==0) {
           last=mid;
        }
         left=mid+1;
      }
  }       
          
         //auto may_last=make_shared<BlockIterator>(shared_from_this(),last,tranc_id);
        // ++*may_last;
   return std::make_pair(std::make_shared<BlockIterator>(shared_from_this(),first,tranc_id),
                     make_shared<BlockIterator>(shared_from_this(),last+1,tranc_id));
}

Block::Entry Block::get_entry_at(size_t offset) const {
  Entry entry;
  entry.key = get_key_at(offset);
  entry.value = get_value_at(offset);
  entry.tranc_id = get_tranc_id_at(offset);
  return entry;
}

size_t Block::size() const { return offsets.size(); }

size_t Block::cur_size() const {
  return data.size() + offsets.size() * sizeof(uint16_t) + sizeof(uint16_t);
}

bool Block::is_empty() const { return offsets.empty(); }

BlockIterator Block::begin(uint64_t tranc_id) {
  // TODO: Lab 3.2 获取begin迭代器
  // ? 返回指向第 0 个 entry 的迭代器: BlockIterator(shared_from_this(), 0,
  // tranc_id)
    return  BlockIterator(shared_from_this(),0,tranc_id);
}

BlockIterator Block::end() {
  // TODO: Lab 3.2 获取end迭代器
  // ? 返回指向末尾 (offsets.size()) 的迭代器
  return BlockIterator(shared_from_this(), offsets.size(), 0);
}
} // namespace tiny_lsm
