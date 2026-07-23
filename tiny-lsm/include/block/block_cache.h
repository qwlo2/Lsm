#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace tiny_lsm {

class Block;

// 定义缓存项
struct CacheItem {
  int sst_id;
  int block_id;
  std::shared_ptr<Block> cache_block;
  uint64_t access_count; // 访问时间戳
};

// 自定义哈希函数
struct pair_hash {
  template <class T1, class T2>
  std::size_t operator()(const std::pair<T1, T2> &p) const {
    auto hash1 = std::hash<T1>{}(p.first);
    auto hash2 = std::hash<T2>{}(p.second);
    return hash1 ^ hash2;
  }
};

// 自定义相等比较函数
struct pair_equal {
  template <class T1, class T2>
  bool operator()(const std::pair<T1, T2> &lhs,
                  const std::pair<T1, T2> &rhs) const {
    return lhs.first == rhs.first && lhs.second == rhs.second;
  }
};

// 定义缓存池
//LRU-k,表示比较第k次的时间戳，LRU极易被范围扫描丢失缓存，因此设置比较第k次，过滤掉范围扫描之类
//<k按LRU处理，>=比较第k次的时间间隔，越近越好，即比较一定时间内频率大小
class BlockCache {
public:
  BlockCache(size_t capacity, size_t k);
  ~BlockCache();

  // 获取缓存项
  std::shared_ptr<Block> get(int sst_id, int block_id);

  // 插入缓存项
  void put(int sst_id, int block_id, std::shared_ptr<Block> data);

  // 获取缓存命中率
  double hit_rate() const;

private:
  using CacheKey = std::pair<int, int>;
  using CacheList = std::list<CacheItem>;
  using CacheMap =
      std::unordered_map<CacheKey, CacheList::iterator, pair_hash, pair_equal>;

  struct CacheShard {
    mutable std::mutex mutex;
    size_t capacity = 0;
    CacheList cache_list_greater_k;
    CacheList cache_list_less_k;
    CacheMap cache_map;
    size_t total_requests = 0;
    size_t hit_requests = 0;
  };

  static constexpr size_t kShardCount = 4;

  size_t capacity_;
  size_t k_;
  std::array<CacheShard, kShardCount> shards_;

  size_t shard_index(int sst_id, int block_id) const;
  void update_access_count(CacheShard &shard, CacheList::iterator it);
};
} // namespace tiny_lsm
