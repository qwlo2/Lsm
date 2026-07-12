#include "sst/sst.h"
#include "config/config.h"
#include "consts.h"
#include "sst/sst_iterator.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>

namespace tiny_lsm {

// Magic byte identifying a WiscKey SST footer
static constexpr uint8_t WISCKEY_MAGIC = 0x4B;
// Old footer size (24 bytes)
static constexpr size_t OLD_FOOTER_SIZE =
    sizeof(uint32_t) * 2 + sizeof(uint64_t) * 2;
// New WiscKey footer size (26 bytes)
static constexpr size_t WISCKEY_FOOTER_SIZE = OLD_FOOTER_SIZE + 2;

// **************************************************
// SST
// **************************************************

std::shared_ptr<SST> SST::open(size_t sst_id, FileObj file,
                               std::shared_ptr<BlockCache> block_cache,
                               std::shared_ptr<VLog> vlog) {
  // TODO: Lab 3.6 打开一个SST文件, 返回一个描述类
  // ? 步骤:
  // ?   0. 检测文件末尾 magic byte 判断是否为 WiscKey 格式 (WISCKEY_MAGIC = 0x4B)
  // ?      footer 共 24 字节 (老格式) 或 26 字节 (WiscKey, 末尾多 storage_mode + magic)
  // ?   1. 从文件末尾读取 footer: meta_block_offset, bloom_offset, min_tranc_id, max_tranc_id
  // ?      如为 WiscKey 格式, 还需读取 storage_mode_
  // ?   2. 读取并解码 Bloom Filter (bloom_offset ~ meta_block_offset 之间)
  // ?   3. 读取并解码元数据块 (meta_block_offset ~ bloom_offset 之间)
  // ?      调用 BlockMeta::decode_meta_from_slice
  // ?   4. 设置 first_key 和 last_key
  // ?   注: vlog 用于 WiscKey 模式下的 value 读取, 直接赋值给 sst->vlog_

  //提取出sst的各个属性，分别mata——section操作block
   auto sst_tmp=std::make_shared<SST>();
  //extea section
    auto file_size=file.size();
    auto tail=file_size;
    auto magic_=file.read_uint8(tail-sizeof(uint8_t));
    auto storage_=file.read_uint8(tail-sizeof(uint16_t));
    //max_txn的最后一个字节也可能等于WISCKEY_MAGIC
    if (magic_==WISCKEY_MAGIC&&storage_==1) {
       tail -=sizeof(uint16_t);
       sst_tmp->storage_mode_=storage_;
    } 
    auto max_txn_id=file.read_uint64(tail-sizeof(uint64_t));
    tail-=sizeof(uint64_t);
    auto min_txn_id=file.read_uint64(tail-sizeof(uint64_t));
    tail-=sizeof(uint64_t);
    auto bloom_offset_=file.read_uint32(tail-sizeof(uint32_t));
    tail-=sizeof(uint32_t);
    auto mata_offset=file.read_uint32(tail-sizeof(uint32_t));
    tail-=sizeof(uint32_t);
    //boolm_section
    auto bloom_section_offset=static_cast<size_t>(bloom_offset_);
    auto  bloom_data_uin8=file.read_to_slice(bloom_section_offset,tail-bloom_section_offset);
     BloomFilter bloom_data;
    if (!bloom_data_uin8.empty()) {
          bloom_data  =std::move(BloomFilter::decode(bloom_data_uin8));
     }
    //mata_section
    auto  meta_section_offset=static_cast<size_t>(mata_offset);
    auto mata_data_uin8=file.read_to_slice(meta_section_offset,bloom_section_offset-meta_section_offset);
    auto mata_data =BlockMeta::decode_meta_from_slice(mata_data_uin8);
    //sst
    sst_tmp->file=std::move(file);
    sst_tmp->first_key=mata_data[0].first_key;
    sst_tmp->last_key=mata_data[mata_data.size()-1].last_key;
    sst_tmp->max_tranc_id_=max_txn_id;
    sst_tmp->min_tranc_id_=min_txn_id;
    sst_tmp->sst_id=sst_id;
    if (bloom_data_uin8.empty()) {
     sst_tmp->bloom_filter=nullptr;
    }
    else { 
        sst_tmp->bloom_filter=std::make_shared<BloomFilter>(bloom_data);
    }
    sst_tmp->bloom_offset=bloom_section_offset;
    sst_tmp->meta_entries=mata_data;
    sst_tmp->meta_block_offset=meta_section_offset;
    sst_tmp->block_cache=block_cache;
    sst_tmp->vlog_=vlog;
     return sst_tmp;
}

void SST::del_sst() { file.del_file(); }

std::shared_ptr<Block> SST::read_block(int64_t block_idx) {
  // TODO: Lab 3.6 根据 block 的 id 读取一个 Block
  // ? 先从 block_cache 查找; 未命中则计算该 block 的偏移和大小
  // ? 读取数据后调用 Block::decode(data, true) 解码
  // ? 解码后存入 block_cache 并返回
  // ? block 大小: 相邻 meta_entries 的 offset 差值; 最后一个 block 到 meta_block_offset
  if (block_idx==-1||block_idx>=meta_entries.size()) {
        return  nullptr;
  }
   if (block_cache) {
       auto block_tmp=block_cache->get(sst_id,block_idx);
       if (block_tmp) {
             return  block_tmp;
       }
   }
     size_t block_size;
   if (block_idx + 1 < static_cast<int64_t>(meta_entries.size())) {
       block_size=meta_entries[block_idx+1].offset-meta_entries[block_idx].offset;
   }else {
    block_size = meta_block_offset - meta_entries[block_idx].offset;
  }
   auto block_data_uin8=file.read_to_slice(meta_entries[block_idx].offset,block_size);
   auto block_data=Block::decode(block_data_uin8);
   if (block_cache) {
     block_cache->put(sst_id,block_idx,block_data);
   }
  return block_data;
}

int64_t SST::find_block_idx(const std::string &key) {
  // TODO: Lab 3.6 二分查找
  // ? 先用布隆过滤器快速排除 (bloom_filter->possibly_contains(key))
  // ? 再在 meta_entries 上二分查找: first_key <= key <= last_key
  // ? 若未找到合适 block 返回 -1
  if (meta_entries.empty()) {
    return -1;
  }
  //暂时
  if (bloom_filter) {
    if (!bloom_filter->possibly_contains(key)) {
        return  -1;
   }
  }
  int left=0,right=meta_entries.size()-1,mid=-1;
  while (left<=right) {
         mid=left+(right-left)/2;
          if (meta_entries[mid].last_key<key) {
               left=mid+1;
            }      
          else {
               right=mid-1;
            }
    }
     if (left >= static_cast<int>(meta_entries.size())) {
          return -1;
    }
       if (meta_entries[left].first_key<=key&&meta_entries[left].last_key>=key) {
              return  left;
       }
  return -1;
}

SstIterator SST::get(const std::string &key, uint64_t tranc_id) {
  // TODO: Lab 3.6 根据查询 key 返回一个迭代器
  // ? 先检查 key 是否在 [first_key, last_key] 范围内, 否则返回 end()
  // ? 再用 bloom_filter 快速排除
  // ? 返回 SstIterator(shared_from_this(), key, tranc_id)
    if (key<first_key||key>last_key) {
        return end();
    }
    //暂时
    if (bloom_filter) {
        if (!bloom_filter->possibly_contains(key)) {
            return end();
        }
    }
      return SstIterator(shared_from_this(), key, tranc_id);
}

size_t SST::num_blocks() const { return meta_entries.size(); }

std::string SST::get_first_key() const { return first_key; }

std::string SST::get_last_key() const { return last_key; }

size_t SST::sst_size() const { return file.size(); }

size_t SST::get_sst_id() const { return sst_id; }

std::string SST::resolve_value(const std::string &raw_value)  const{
  // WiscKey 模式下: raw_value 是 12 字节的 vlog 引用 [offset:8][size:4]
  // 普通模式下直接返回 raw_value
  if (storage_mode_ == 0 || raw_value.empty()) {
    return raw_value;
  }
  //超过100万会出错
  //原来的实验无法区分12的数据和引用
  if (raw_value.size() < 12) {
    return raw_value;
  }
  uint64_t off = 0;
  uint32_t sz = 0;
  memcpy(&off, raw_value.data(), sizeof(uint64_t));
  memcpy(&sz, raw_value.data() + sizeof(uint64_t), sizeof(uint32_t));
  if (!vlog_) {
    throw std::runtime_error("SST::resolve_value: vlog is null for WiscKey SST");
  }
  return vlog_->read_value(off, sz);
}

bool SST::is_wisckey() const { return storage_mode_ == 1; }

SstIterator SST::begin(uint64_t tranc_id, bool keep_all_versions) {
  // TODO: Lab 3.6 返回起始位置迭代器
  // ? 返回 SstIterator(shared_from_this(), tranc_id, keep_all_versions)
   return SstIterator(shared_from_this(), tranc_id, keep_all_versions);
}

SstIterator SST::end() {
  // TODO: Lab 3.6 返回终止位置迭代器
  // ? 构造一个 SstIterator 并将 m_block_idx 设为 meta_entries.size(), m_block_it 设为 nullptr
     auto it=SstIterator();
     it.set_block_idx(meta_entries.size());
     it.set_block_it(nullptr);
     return it;
}

std::pair<uint64_t, uint64_t> SST::get_tranc_id_range() const {
  return std::make_pair(min_tranc_id_, max_tranc_id_);
}

// **************************************************
// SSTBuilder
// **************************************************

SSTBuilder::SSTBuilder(size_t block_size, bool has_bloom) : block(block_size),block_size(block_size) {
  // 初始化第一个block
  if (has_bloom) {
    bloom_filter = std::make_shared<BloomFilter>(
        TomlConfig::getInstance().getBloomFilterExpectedSize(),
        TomlConfig::getInstance().getBloomFilterExpectedErrorRate());
  }
  meta_entries.clear();
  data.clear();
  first_key.clear();
  last_key.clear();
}

SSTBuilder::SSTBuilder(size_t block_size, bool has_bloom,
                       std::shared_ptr<VLog> vlog,
                       size_t wisckey_threshold)
    : block(block_size), block_size(block_size),vlog_(std::move(vlog)),
      wisckey_threshold_(wisckey_threshold), storage_mode_(1) {
  // WiscKey 模式构造函数: vlog 用于大 value 分离存储
  if (has_bloom) {
    bloom_filter = std::make_shared<BloomFilter>(
        TomlConfig::getInstance().getBloomFilterExpectedSize(),
        TomlConfig::getInstance().getBloomFilterExpectedErrorRate());
  }
  meta_entries.clear();
  data.clear();
  first_key.clear();
  last_key.clear();
}

void SSTBuilder::add(const std::string &key, const std::string &value,
                     uint64_t tranc_id) {
  // TODO: Lab 3.5 添加键值对
  // ? 记录 first_key (第一次调用时)
  // ? 向 bloom_filter 中 add key
  // ? 更新 max_tranc_id_ / min_tranc_id_
  // ? WiscKey 模式下: 若 value 非空且超过 wisckey_threshold_, 将 value 写入 vlog
  // ?   并将 vlog 引用 [offset:8][size:4] 作为 actual_value
  // ? 尝试向 block 添加 entry; 若返回 false (block满) 先调用 finish_block() 再添加
  // ? 注意: 相同 key 必须在同一个 block 中 (force_write = key == last_key)
  // ? 更新 last_key
   if (last_key.empty()) {
       first_key=key;
   }  
   if (bloom_filter) {
      bloom_filter->add(key);
   }
   max_tranc_id_=std::max(max_tranc_id_,tranc_id);
   min_tranc_id_=std::min(min_tranc_id_,tranc_id);
    bool add_vaild=false;
    //存疑
    //由于<=时，compact时会再次append，造成引用混乱
    //而>时，read分不清12的元数据和引用
    //因此在commit时同步实现对>=12的写入vlog
  //  if (vlog_ && storage_mode_ == 1 && value.size() >wisckey_threshold_) {
  //      auto offset=vlog_->append(key,value);
  //      std::string vlog_ref(sizeof(uint64_t) + sizeof(uint32_t), '\0');
  //      memcpy(vlog_ref.data(), &offset, sizeof(uint64_t));
  //      uint32_t val_sz=static_cast<uint32_t>(value.size());
  //      memcpy(vlog_ref.data()+sizeof(uint64_t), &val_sz, sizeof(uint32_t));
  //       //实际写入sst的value
  //       add_vaild=block.add_entry(key,vlog_ref, tranc_id,key==last_key);
  //  }else {
  //        add_vaild=block.add_entry(key,value, tranc_id,key==last_key);
  //  }
    add_vaild=block.add_entry(key,value, tranc_id,key==last_key);
   if (!add_vaild) {
       finish_block();
       add_vaild=block.add_entry(key,value, tranc_id,key==last_key);
  }
  //每次读更新，在mata_entry中做last_key，get——first——key做f——key
  last_key=key;
 }

size_t SSTBuilder::real_size() const { return data.size() + block.cur_size(); }

size_t SSTBuilder::estimated_size() const { return data.size(); }

void SSTBuilder::finish_block() {
  // TODO: Lab 3.5 构建块
  // ? 将当前 block 编码并追加到 data, 同时向 meta_entries 添加元数据
  // ? 然后重置 block 为新的空 Block
  // ? meta_entries 记录: (当前data起始偏移, first_key, last_key)
  if (block.is_empty()) {
       return;
  }
    auto block_data= block.encode();
    //记录meta——entries
     meta_entries.emplace_back(data.size(),block.get_first_key(),last_key);
     //插入block section
     data.insert(data.end(), block_data.begin(),block_data.end());
      block=Block(block_size);
}

std::shared_ptr<SST>
SSTBuilder::build(size_t sst_id, const std::string &path,
                  std::shared_ptr<BlockCache> block_cache) {
  // TODO: Lab 3.5 构建一个SST
  // ? 1. 若 block 非空则调用 finish_block()
  // ? 2. 若 meta_entries 为空则抛出异常
  // ? 3. 编码元数据块并追加到 data (BlockMeta::encode_meta_to_slice)
  // ? 4. 追加 Bloom Filter 编码
  // ? 5. 写入 footer (老格式 24B 或 WiscKey 26B):
  // ?    [meta_offset:uint32][bloom_offset:uint32][min_tranc_id:uint64][max_tranc_id:uint64]
  // ?    WiscKey 额外: [storage_mode_:uint8][WISCKEY_MAGIC:uint8]
  // ? 6. 调用 FileObj::create_and_write 写文件
  // ? 7. 构造并返回 SST 对象
  //Bloom Filter后面的实现
  if (!block.is_empty()) {
        finish_block();
  }
  if (meta_entries.empty()) {
      throw  std::runtime_error("meta_entries is empty");
  }
  //mata——section
   uint32_t mata_section_of=data.size();

  //写入mata_entries
  BlockMeta::encode_meta_to_slice(meta_entries,data);
   
 //bloom_section,后面再写入
   uint32_t bloom_of=data.size();
   std::vector<uint8_t> bloom_data;
   if (bloom_filter) {
       bloom_data=bloom_filter->encode();
       data.insert(data.end(), bloom_data.begin(), bloom_data.end());
   }
    
  //封装了对文件的操作
   auto file= FileObj::create_and_write(path,data);
  //sst的extra section
  file.append_uint32(mata_section_of);
  file.append_uint32(bloom_of);
  file.append_uint64(min_tranc_id_);
  file.append_uint64(max_tranc_id_);
  //WiscKey 额外: [storage_mode_:uint8][WISCKEY_MAGIC:uint8]
  if (storage_mode_) {
       file.append_uint8(storage_mode_);
       file.append_uint8(WISCKEY_MAGIC);
  }
  //sst写入磁盘
   file.sync();
   //vlog写入磁盘
    if (storage_mode_&&vlog_) {
        vlog_->sync();
    }
   //没有显示构造
   auto sst = std::make_shared<SST>();
   sst->file=std::move(file);
   sst->meta_entries = meta_entries;
    sst->bloom_offset =bloom_of;
    sst->bloom_filter=bloom_filter;
   sst->meta_block_offset =mata_section_of;
   sst->sst_id = sst_id;
   sst->first_key = first_key;
   sst->last_key = last_key;
   sst->block_cache = block_cache;
   sst->max_tranc_id_=max_tranc_id_;
   sst->min_tranc_id_=min_tranc_id_;
   sst->storage_mode_=storage_mode_;
   sst->vlog_ = vlog_;
   return sst;
}
} // namespace tiny_lsm
