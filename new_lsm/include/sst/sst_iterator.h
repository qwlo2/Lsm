#pragma once
#include "block/block_iterator.h"
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace tiny_lsm {

class SstIterator;
class SST;
//sst版本的没有知道txn——id
std::optional<std::pair<SstIterator, SstIterator>>
sst_iters_monotony_predicate(std::shared_ptr<SST> sst, uint64_t tranc_id,
                             std::function<int(const std::string &)> predicate);

class SstIterator : public BaseIterator {
  friend std::optional<std::pair<SstIterator, SstIterator>>
  sst_iters_monotony_predicate(
      std::shared_ptr<SST> sst, uint64_t tranc_id,
      std::function<int(const std::string &)> predicate);

  friend SST;

private:
  std::shared_ptr<SST> m_sst;
  int64_t m_block_idx;
  uint64_t max_tranc_id_;
  std::shared_ptr<BlockIterator> m_block_it;
  mutable std::optional<value_type> cached_value; // 缓存当前值
  bool keep_all_versions_ = false;

  void update_current() const;
  void set_block_idx(size_t idx);
  void set_block_it(std::shared_ptr<BlockIterator> it);
  void set_m_sst(std::shared_ptr<SST> sst){
     m_sst=sst;
  }
public:
  SstIterator( uint64_t tranc_id=0,bool keep_all_versions = false)
    : m_sst(nullptr),
      m_block_idx(0),
      max_tranc_id_(tranc_id),
      m_block_it(nullptr),
      cached_value(std::nullopt) ,
      keep_all_versions_(keep_all_versions){}
  // 创建迭代器, 并移动到第一个key
  SstIterator(std::shared_ptr<SST> sst, uint64_t tranc_id,
              bool keep_all_versions = false);
  // 创建迭代器, 并移动到第指定key
  SstIterator(std::shared_ptr<SST> sst, const std::string &key,
              uint64_t tranc_id, bool keep_all_versions = false);

  // 创建迭代器, 并移动到第指定前缀的首端或者尾端
  //没有实现， sst_iters_monotony_predicate1代替了
  static std::optional<std::pair<SstIterator, SstIterator>>
  iters_monotony_predicate(std::shared_ptr<SST> sst, uint64_t tranc_id,
                           std::function<bool(const std::string &)> predicate);

  void seek_first();
  void seek(const std::string &key);
  std::string key();
  std::string value();
  //block的构造有skip，要对block-it进行is-end判断，再++跳到第一个有效的
  virtual BaseIterator &operator++() override;
  virtual bool operator==(const BaseIterator &other) const override;
  virtual bool operator!=(const BaseIterator &other) const override;
  virtual value_type operator*() const override;
  virtual IteratorType get_type() const override;
  virtual uint64_t get_tranc_id() const override;
  virtual bool is_end() const override;
  virtual bool is_valid() const override;
  //只是补丁，block的迭代器已经skip了
  void skip_by_tranc_id();
  pointer operator->() const;
  uint64_t get_cur_tranc_id() const;

  static std::pair<HeapIterator, HeapIterator>
  merge_sst_iterator(std::vector<SstIterator> iter_vec, uint64_t tranc_id,
                     bool keep_all_versions = false);
};
} // namespace tiny_lsm