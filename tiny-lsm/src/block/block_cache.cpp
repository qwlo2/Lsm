#include "block/block_cache.h"
#include "block/block.h"
#include <cstddef>
#include <list>
#include <memory>
#include <mutex>
#include <utility>

namespace tiny_lsm {
BlockCache::BlockCache(size_t capacity, size_t k)
    : capacity_(capacity), k_(k) {
  const size_t base_capacity = capacity_ / kShardCount;
  const size_t remainder = capacity_ % kShardCount;
  for (size_t i = 0; i < kShardCount; ++i) {
    shards_[i].capacity = base_capacity + (i < remainder ? 1 : 0);
  }
}

BlockCache::~BlockCache() = default;

std::shared_ptr<Block> BlockCache::get(int sst_id, int block_id) {
  auto &shard = shards_[shard_index(sst_id, block_id)];
  std::lock_guard<std::mutex> lock(shard.mutex);
  auto try_find = shard.cache_map.find(std::make_pair(sst_id, block_id));
  ++shard.total_requests;
  if (try_find == shard.cache_map.end()) {
    return nullptr;
  }

  auto block = try_find->second->cache_block;
  update_access_count(shard, try_find->second);
  ++shard.hit_requests;
  return block;
}
void BlockCache::put(int sst_id, int block_id, std::shared_ptr<Block> block) {
  auto &shard = shards_[shard_index(sst_id, block_id)];
  std::lock_guard<std::mutex> lock(shard.mutex);
  const auto key = std::make_pair(sst_id, block_id);

  if (shard.cache_map.find(key) != shard.cache_map.end() ||
      shard.capacity == 0) {
    return;
  }

  if (shard.cache_map.size() >= shard.capacity) {
    if (!shard.cache_list_less_k.empty()) {
      const auto &victim = shard.cache_list_less_k.back();
      shard.cache_map.erase(std::make_pair(victim.sst_id, victim.block_id));
      shard.cache_list_less_k.pop_back();
    } else {
      const auto &victim = shard.cache_list_greater_k.back();
      shard.cache_map.erase(std::make_pair(victim.sst_id, victim.block_id));
      shard.cache_list_greater_k.pop_back();
    }
  }

  auto &target_list =
      k_ <= 1 ? shard.cache_list_greater_k : shard.cache_list_less_k;
  target_list.emplace_front(sst_id, block_id, std::move(block), 1);
  shard.cache_map.emplace(key, target_list.begin());
}

double BlockCache::hit_rate() const {
  size_t total_requests = 0;
  size_t hit_requests = 0;
  for (const auto &shard : shards_) {
    std::lock_guard<std::mutex> lock(shard.mutex);
    total_requests += shard.total_requests;
    hit_requests += shard.hit_requests;
  }

  if (total_requests == 0) {
    return 0;
  }
  return hit_requests * 1.0 / total_requests;
}

size_t BlockCache::shard_index(int sst_id, int block_id) const {
  return pair_hash{}(std::make_pair(sst_id, block_id)) % kShardCount;
}

void BlockCache::update_access_count(CacheShard &shard,
                                     CacheList::iterator it) {
  const bool was_hot = it->access_count >= k_;
  ++it->access_count;

  if (was_hot) {
    shard.cache_list_greater_k.splice(shard.cache_list_greater_k.begin(),
                                      shard.cache_list_greater_k, it);
  } else if (it->access_count >= k_) {
    shard.cache_list_greater_k.splice(shard.cache_list_greater_k.begin(),
                                      shard.cache_list_less_k, it);
  } else {
    shard.cache_list_less_k.splice(shard.cache_list_less_k.begin(),
                                   shard.cache_list_less_k, it);
  }
}

} // namespace tiny_lsm
