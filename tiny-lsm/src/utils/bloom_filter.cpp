// include/utils/bloom_filter.cpp

#include "utils/bloom_filter.h"
#include "config/config.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace tiny_lsm {

BloomFilter::BloomFilter() {};

// 构造函数，初始化布隆过滤器
// expected_elements: 预期插入的元素数量
// false_positive_rate: 允许的假阳性率
BloomFilter::BloomFilter(size_t expected_elements, double false_positive_rate)
    : expected_elements_(expected_elements),
      false_positive_rate_(false_positive_rate) {
        //ceil向上取整
  num_bits_ = static_cast<size_t>(
      std::ceil(-static_cast<double>(expected_elements_) *
                std::log(false_positive_rate_) /
                std::pow(std::log(2), 2)));
  num_hashes_ = static_cast<size_t>(
      std::ceil(static_cast<double>(num_bits_) / expected_elements_ *
                std::log(2)));
  bits_.resize(num_bits_, false);
}

void BloomFilter::add(const std::string &key) {
        for (size_t i=num_hashes_; i>0;--i) {
             bits_[hash(key,i)%num_bits_]=true;
       }
}

//  如果key可能存在于布隆过滤器中，返回true；否则返回false
bool BloomFilter::possibly_contains(const std::string &key) const {
      for (size_t i=num_hashes_; i>0;--i) {
             if (bits_[hash(key,i)%num_bits_]==false) {
                  return false;
             }
       }
       return true;
}

// 清空布隆过滤器
void BloomFilter::clear() { bits_.assign(bits_.size(), false); }

size_t BloomFilter::hash1(const std::string &key) const {
  std::hash<std::string> hasher;
  return hasher(key);
}

size_t BloomFilter::hash2(const std::string &key) const {
  std::hash<std::string> hasher;
  return hasher(key + "salt");
}

size_t BloomFilter::hash(const std::string &key, size_t idx) const {
       //idx代表第n个hash，需要用线性组合出idx个
       return (hash2(key)^hash1(key)^idx)*idx+1;
}

// 编码布隆过滤器为 std::vector<uint8_t>
std::vector<uint8_t> BloomFilter::encode() {
  std::vector<uint8_t> ans((num_bits_ + 7) / 8, 0);
  for (size_t i = 0; i < num_bits_; ++i) {
    if (bits_[i]) {
      ans[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
    }
  }

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
  auto crc=crc32(ans);
   for (int i=0;i<sizeof(uint32_t);++i) {
        ans.emplace_back(static_cast<uint32_t>(crc>>(i*8)));
   }
     return ans;
}

// 从 std::vector<uint8_t> 解码布隆过滤器
BloomFilter BloomFilter::decode(const std::vector<uint8_t> &data) {
     BloomFilter bloom(TomlConfig::getInstance().getBloomFilterExpectedSize(),
                     TomlConfig::getInstance().getBloomFilterExpectedErrorRate());
     auto crc32=[](std::vector<uint8_t> data){
           uint32_t crc=0xffffffff;
           for (int it=0;it<data.size()-4;++it) {
              crc ^= data[it];
                for (int i=0;i<8;++i) {
                  if (crc & 1) {
                    crc = (crc >> 1) ^ 0xEDB88320;
                  } else {
                    crc >>= 1;
                    }
                }
           }
           return crc ^ 0xffffffff;
     };
     uint32_t save;
     memcpy(&save,data.data()+data.size()-4,sizeof(uint32_t));
     auto crc=crc32(data);
    if (save!=crc) {
       throw  std::runtime_error("crc cherc error");
    }
    // size_t i=0,j=0,bit_num=bloom.bits_.size();
    // for (auto& it : data) {
    //      for (;j<8&&i<bit_num;++i,++j) {
    //       bloom.bits_[i]=static_cast<bool>(it>>(j%8)&1u);
    //      }
    //      j=0;
    // }
    size_t bit_num = bloom.bits_.size();
    size_t byte_count = data.size() - sizeof(uint32_t);

    for (size_t byte_idx = 0; byte_idx < byte_count; ++byte_idx) {
        //uint8_t byte = data[byte_idx];
            for (size_t bit = 0; bit < 8; ++bit) {
                size_t idx = byte_idx * 8 + bit;
                  if (idx >= bit_num) {
                        break;
              }
    bloom.bits_[idx] = static_cast<bool>(( data[byte_idx] >> bit) & 1u);
  }
}
    return bloom;
}
} // namespace tiny_lsm