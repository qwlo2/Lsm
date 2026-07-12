#include "block/block_cache.h"
#include "block/block.h"
#include "sst/sst.h"
#include <chrono>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace tiny_lsm {
BlockCache::BlockCache(size_t capacity, size_t k)
    : capacity_(capacity), k_(k) {}

BlockCache::~BlockCache() = default;

std::shared_ptr<Block> BlockCache::get(int sst_id, int block_id) {
     std::unique_lock<std::mutex> lock(mutex_);
      auto try_find=cache_map_.find(std::make_pair(sst_id, block_id));
        ++total_requests_;
      if (try_find!=cache_map_.end()) {
           update_access_count(try_find->second);
            ++hit_requests_;
            return cache_map_.find(std::make_pair(sst_id, block_id))->second->cache_block;
      }
      return  nullptr;
}

void BlockCache::put(int sst_id, int block_id, std::shared_ptr<Block> block) {
      std::unique_lock<std::mutex> lock(mutex_);
       if (auto tmp=cache_map_.find(std::make_pair(sst_id, block_id));tmp!=cache_map_.end()) {
                return;
       }
       if (capacity_<=cache_list_greater_k.size()+cache_list_less_k.size()) {
              if (cache_list_less_k.size()!=0) {
                 cache_map_.erase(std::move(std::make_pair(cache_list_less_k.back().sst_id,
                                 cache_list_less_k.back().block_id)));
                    cache_list_less_k.pop_back();
              }else {
                 cache_map_.erase(std::move(std::make_pair(cache_list_greater_k.back().sst_id,
                                 cache_list_greater_k.back().block_id)));
                cache_list_greater_k.pop_back();
              }
       }
       cache_list_less_k.emplace_front(sst_id,block_id,block,0);
       update_access_count(cache_list_less_k.begin());
      
}    

double BlockCache::hit_rate() const {
     if (!total_requests_||!hit_requests_) {
         return 0;
     }
     return  hit_requests_*1.0/total_requests_;
}

void BlockCache::update_access_count(std::list<CacheItem>::iterator it) {
     bool was_greater = it->access_count >= k_;
     ++it->access_count;
     auto key=std::make_pair(it->sst_id,it->block_id);
      auto item = *it;
        if (it->access_count>=k_) {
         // --it->access_count>=k_，这个会把值改回去
            if (was_greater) {
              cache_list_greater_k.erase(it);
            }else {
               cache_list_less_k.erase(it);
            }
            // cache_map_.erase(std::move(std::make_pair(it->sst_id,it->block_id)));
             cache_list_greater_k.emplace_front(item);
             cache_map_[key]=cache_list_greater_k.begin();
        }
        else {
           cache_list_less_k.erase(it);
           //cache_map_.erase(std::move(std::make_pair(it->sst_id,it->block_id)));
           cache_list_less_k.emplace_front(item);
           cache_map_[key]=cache_list_less_k.begin();
        }
          
        
} // namespace tiny_lsm
}