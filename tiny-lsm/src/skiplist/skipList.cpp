#include "skiplist/skiplist.h"
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace tiny_lsm {

// ************************ SkipListIterator ************************
BaseIterator &SkipListIterator::operator++() {
  // TODO: Lab1.2 任务：实现SkipListIterator的++操作符
  // ? current 是当前节点指针, forward_[0] 是最底层链表的下一个节点
  if (current) {
    current = current->forward_[0];
  }
  return *this;
}

bool SkipListIterator::operator==(const BaseIterator &other) const {
  // TODO: Lab1.2 任务：实现SkipListIterator的==操作符
  // ? 需要先通过 get_type() 判断类型再做 dynamic_cast
  if (other.get_type() == get_type()) {
    const auto &other_it = dynamic_cast<const SkipListIterator &>(other);
    if (is_end() && other_it.is_end()) {
      return true;
    } else if (is_end() || other_it.is_end()) {
      return false; // 必须，不然！=end（）报错，other_it.get_key() 会报错
    } else if (get_key() == other_it.get_key() &&
               get_value() == other_it.get_value() &&
               get_tranc_id() == other_it.get_tranc_id()) {
      return true;
    }
  }
  return false;
}

bool SkipListIterator::operator!=(const BaseIterator &other) const {
  // TODO: Lab1.2 任务：实现SkipListIterator的!=操作符
  return !(*this == other);
}

SkipListIterator::value_type SkipListIterator::operator*() const {
  // TODO: Lab1.2 任务：实现SkipListIterator的*操作符
  // ? 若 current 为空需抛出异常
  if (!current) {
    throw std::runtime_error("iterator is end or nullptr");
  }
  return {get_key(), get_value()};
}

IteratorType SkipListIterator::get_type() const {
  // TODO: Lab1.2 任务：实现SkipListIterator的get_type
  // ? 主要是为了熟悉基类的定义和继承关系, 返回 IteratorType::SkipListIterator

  return IteratorType::SkipListIterator; // placeholder, 请替换为正确实现
}

bool SkipListIterator::is_valid() const {
  return current && !current->entry.key.empty();
}
bool SkipListIterator::is_end() const { return current == nullptr; }

std::string SkipListIterator::get_key() const { return current->entry.key; }
std::string SkipListIterator::get_value() const { return current->entry.value; }
uint64_t SkipListIterator::get_tranc_id() const { return current->entry.tranc_id; }
EntryState SkipListIterator::get_effective_state() const {
  return current->entry.effective_state();
}

// ************************ SkipList ************************
// 构造函数
SkipList::SkipList(int max_lvl) : max_level(max_lvl), current_level(1) {
  head = std::make_shared<SkipListNode>(max_level);
  dis_01 = std::uniform_int_distribution<>(0, 1);
  dis_level = std::uniform_int_distribution<>(0, (1 << max_lvl) - 1);
  gen = std::mt19937(std::random_device()());
}
// ? 通过"抛硬币"的方式随机生成层数：
// ? - 每次有50%的概率增加一层
// ? - 确保层数分布为：第1层100%，第2层50%，第3层25%，以此类推
// ? - 层数范围限制在[1, max_level]之间，避免浪费内存
// TODO: Lab1.1 任务：插入时随机为这一次操作确定其最高连接的链表层数
int SkipList::random_level() {
  int ran = dis_01(gen), level = 0;
  while (ran && level < max_level - 1) {
    level++;
    ran = dis_01(gen);
  }
  return level + 1; // 返回的是有几层，node的vector（n，value）
  //     int ran = dis_level(gen);
  //     while (ran&1&&level<max_level-1) {
  //         level++;
  //         ran>>=1;
  //     }
}

// 插入或更新键值对
// TODO: Lab1.1 任务：实现插入或更新键值对
// ? Hint: 你需要保证不同`Level`的步长从底层到高层逐渐增加
// ? 你可能需要使用到`random_level`函数以确定层数, 其注释中为你提供一种思路
// ? tranc_id 为事务id, 直接将其传递到 SkipListNode 的构造函数中即可
// ? 若key存在且tranc_id相同, 仅更新value; 否则插入新节点
// ? 注意维护 size_bytes
void SkipList::put(const std::string &key, const std::string &value,
                   uint64_t ts, EntryState state,
                   std::shared_ptr<SharedEntryState> shared_state) {
  auto prev = head;
  std::vector<std::shared_ptr<SkipListNode>> update(max_level);
   int level=random_level();
   auto e=Entry(key,value,ts,state,std::move(shared_state));
  auto node = std::make_shared<SkipListNode>(e,level);
  for (int i = current_level-1; i >= 0; i--) {
    while (prev->forward_[i] &&*prev->forward_[i] < *node) { // 直接比较节点，txn_id大的在前面
            prev = prev->forward_[i];
    }
      update[i]=prev;
  }
  // 同一事务对同一 key 的最终已提交版本也采用追加写。
  // RU 的旧未提交版本由共享事务状态整体失效，避免原地修改节点与并发读竞争。

  if (level > current_level) {
  for (int i = current_level; i < level; ++i) {
    update[i] = head;
  }
  current_level = level;
}
  for (int i=level-1;i>=0;i--) {
        auto temp=update[i];
          if (temp->forward_[i]) {
         temp->forward_[i]->backward_[i] = node;
         } // 悬空
      node->forward_[i] = temp->forward_[i];
       temp->forward_[i] = node;
      node->backward_[i] = temp;
       
  }
  size_bytes += key.size() + value.size() + sizeof(ts);
 }


// void SkipList::put(const std::string &key, const std::string &value,
//                    uint64_t tranc_id) {
//   spdlog::trace("SkipList--put({}, {}, {})", key, value, tranc_id);
//   // char数组初始化" "，带\0，【】逐个没有
//   //" "这样的字符串会自动加\0，string没有，c_str(),date（）兼容c返回的带\0，
//   // 存入磁盘中是只需key，value，txn-id

//   int level = random_level();
//   if (level > current_level) {
//     current_level = level;
//   }
//   auto node = std::make_shared<SkipListNode>(key, value, level, tranc_id);
//   auto prev = head;
//   bool add_bytes = false; // 是否已经增加size_bytes
//   for (int i = level - 1; i >= 0; i--) {
//     while (prev->forward_[i] &&*prev->forward_[i] < *node) { //
//     直接比较节点，txn_id大的在前面
//       prev = prev->forward_[i];
//     }
//     if (prev->forward_[i] && prev->forward_[i]->key_ == key &&
//         prev->forward_[i]->tranc_id_ == tranc_id) {
//       // value and tranc_id equal, update value
//       //
//       skiplist的迭代器没有重载->,所以无法修改和访问节点的成员变量，因此不用get
//       // 只需修改一次
//       size_bytes += value.size() - prev->forward_[i]->value_.size();
//       prev->forward_[i]->value_ = node->value_;
//       break;
//     } else {
//       if (prev->forward_[i]) {
//         prev->forward_[i]->backward_[i] = node;
//       } // 悬空
//       node->forward_[i] = prev->forward_[i];
//       prev->forward_[i] = node;
//       node->backward_[i] = prev;
//       if (!add_bytes) {
//         size_bytes += key.size() + value.size() + sizeof(tranc_id);
//         add_bytes = true;
//       }
//     }
//   }
// }

// TODO: Lab1.1 任务：实现查找键值对
// ? 从最高层开始向下查找, 最终在底层确认 key 是否存在
// ? 若 tranc_id == 0, 直接比较 key 返回; 否则需满足事务可见性 (tranc_id_ <=
// tranc_id)
// TODO: 完成查找后还需要额外实现SkipListIterator中的TODO部分(Lab1.2)
// 查找键值对
// 确认下一个节点>=key，true，则向下一层继续比较，false，则当前层继续比较下一个节点，直到找到key或者到达底层
SkipListIterator SkipList::get(const std::string &key, uint64_t tranc_id,
                               ReadVisibility visibility) {
  spdlog::trace("SkipList--get({}) called", key);
  auto current = head;

  for (int i = current_level - 1; i >= 0; i--) {
    while (current->forward_[i] &&
           current->forward_[i]->entry.key < key) {
      current = current->forward_[i];
    }
  }

  auto candidate = current->forward_[0];
  while (candidate && candidate->entry.key == key) {
    const bool txn_visible =
        tranc_id == 0 || candidate->entry.tranc_id <= tranc_id;
    if (txn_visible && is_state_visible(candidate->entry, visibility)) {
      return SkipListIterator(candidate);
    }
    candidate = candidate->forward_[0];
  }

  return SkipListIterator{};
}

// 删除键值对
// ! 这里的 remove 是跳表本身真实的 remove,  lsm 应该使用 put 空值表示删除,
// ! 这里只是为了实现完整的 SkipList 不会真正被上层调用
// TODO: Lab1.1 任务：实现删除键值对
// ? 从最高层开始查找目标节点并更新各层指针
// ? 注意同时维护 backward_ 指针和 size_bytes
// void SkipList::remove(const std::string &key) {
//     auto temp = head;
//   std::vector<std::shared_ptr<SkipListNode>> update(max_level);
//   for (int i = current_level-1; i >= 0; i++) {
//     while (temp->forward_[i] &&temp->forward_[i]->key_<key) { // 直接比较节点，txn_id大的在前面
//            temp = temp->forward_[i];
//     }
//       update[i]=temp;
//   }
//   //level0无则返回
//   auto target = update[0]->forward_[0];
//   if (!target || target->key_ != key) {
//     return;
//   }

//   for (int i=current_level-1;i>=0;i--) {
//       auto  prev=update[i];
//          if (prev->forward_[i]&&prev->forward_[i]->key_==key) {
//                auto last=prev->forward_[i];
//                while (last->forward_[i]&&last->forward_[i]->key_==key) {
//                     last=last->forward_[i];
//                }
//                if (last->forward_[i]) {
//                      last->forward_[i]->backward_[i]=prev;
//                }
//                prev->forward_[i]=last->forward_[i];
//          }
//   }
//  while (current_level > 1 && !head->forward_[current_level - 1]) {
//       current_level--;
//     }
// txn_id 欠缺


void SkipList::remove(const std::string &key) {
  auto current = head;
  for (int i = current_level - 1; i >= 0; i--) {
    while (current->forward_[i] && current->forward_[i]->entry.key < key) {
      current = current->forward_[i];
    }
    // mvcc，tranc_id_ <= tranc_id存疑问（seq,mvcc）
    while (current->forward_[i] && current->forward_[i]->entry.key == key) {
      auto next = current->forward_[i];
      size_bytes -=
          next->entry.key.size() + next->entry.value.size() + sizeof(next->entry.tranc_id);
      for (int j = next->forward_.size() - 1; j >= 0; j--) {
        if (next->forward_[j]) {
          next->forward_[j]->backward_[j] = next->backward_[j].lock();
        }
        next->backward_[j].lock()->forward_[j] = next->forward_[j];
      }
    }
    while (current_level > 1 && !head->forward_[current_level - 1]) {
      current_level--;
    }
  }
}

// 刷盘时可以直接遍历最底层链表
std::vector<std::tuple<std::string, std::string, uint64_t>> SkipList::flush() {
  // std::shared_lock<std::shared_mutex> slock(rw_mutex);
  spdlog::debug("SkipList--flush(): Starting to flush skiplist data");

  std::vector<std::tuple<std::string, std::string, uint64_t>> data;
  auto node = head->forward_[0];
  while (node) {
    if (node->entry.effective_state() == EntryState::COMMITTED) {
      data.emplace_back(node->entry.key, node->entry.value, node->entry.tranc_id);
    }
    node = node->forward_[0];
  }

  spdlog::debug("SkipList--flush(): Flushed {} entries", data.size());

  return data;
}

size_t SkipList::get_size() {
  // std::shared_lock<std::shared_mutex> slock(rw_mutex);
  return size_bytes;
}

// 清空跳表，释放内存
void SkipList::clear() {
  // std::unique_lock<std::shared_mutex> lock(rw_mutex);
  head = std::make_shared<SkipListNode>( max_level);
  size_bytes = 0;
}

SkipListIterator SkipList::begin() {
  // return SkipListIterator(head->forward[0], rw_mutex);
  return SkipListIterator(head->forward_[0]);
}

SkipListIterator SkipList::end() {
  return SkipListIterator(); // 使用空构造函数
}

// 找到前缀的起始位置
// 返回第一个前缀匹配或者大于前缀的迭代器
SkipListIterator SkipList::begin_preffix(const std::string &preffix) {
  // TODO: Lab1.3 任务：实现前缀查询的起始位置
  // ? 从最高层开始查找, 找到第一个 key >= preffix 的节点
  auto prev = head;
  for (int i = current_level - 1; i >= 0; i--) {
    while (prev->forward_[i] && prev->forward_[i]->entry.key < preffix) {
      prev = prev->forward_[i];
    }
  }
  // prev->forward_[i]->key_ .compare(0, preffix.size(), preffix) == 0
  // prev->forward_[i]->key_.starts_with(preffix)
  return SkipListIterator(prev->forward_[0]);
}

// 找到前缀的终结位置
SkipListIterator SkipList::end_preffix(const std::string &prefix) {
  // TODO: Lab1.3 任务：实现前缀查询的终结位置
  // ? 找到第一个 key 不以 prefix 开头的节点作为终结位置
  auto prev = head;
  for (int i = current_level - 1; i >= 0; i--) {
    while (prev->forward_[i] && prev->forward_[i]->entry.key < prefix) {
      prev = prev->forward_[i];
    }
  }
  while (prev->forward_[0] && prev->forward_[0]->entry.key.starts_with(prefix)) {
    prev = prev->forward_[0];
  }
  return SkipListIterator(prev->forward_[0]);
}

// ? 这里单调谓词的含义是, 整个数据库只会有一段连续区间满足此谓词
// ? 例如之前特化的前缀查询，以及后续可能的范围查询，都可以转化为谓词查询
// ? 返回第一个满足谓词的位置和最后一个满足谓词的迭代器
// ? 如果不存在, 返回 nullopt
// ? 谓词作用于key, 且保证满足谓词的结果只在一段连续的区间内,
// 例如前缀匹配的谓词 ? predicate返回值: ?   0: 满足谓词 ?   >0: 不满足谓词,
// 需要向右移动 ?   <0: 不满足谓词, 需要向左移动 ! Skiplist
// 中的谓词查询不会进行事务id的判断, 需要上层自己进行判断

// 普通的范围查询、前缀匹配或者是其他的单条区间查询（例如大于等于某个值的查询）
// std::optional一个可能包含或不包含值的对象
// std::optional<std::pair<SkipListIterator, SkipListIterator>>
// SkipList::iters_monotony_predicate(
//     std::function<int(const std::string &)> predicate) {
// TODO: Lab1.3 任务：实现谓词查询
// ? 分两步: 1. 利用多层跳表快速找到谓词满足区间内的一个节点
// ?         2. 分别向前/向后扩展, 利用 backward_ 和 forward_ 确定区间边界
// ? 注意: 向前查找时需要利用 backward_ 指针从当前节点的最高层开始回溯
//   auto prev = head;
//   for (int i = current_level - 1; i >= 0; i--) {
//=0,匹配，>0（1）,需要向右移动，<0（-1）,需要向左移动
// 谓词大了，向右移动
//     while (prev->forward_[i] && predicate(prev->forward_[i]->key_) > 0) {
//       prev = prev->forward_[i];
//     }
//   }
//   if (prev->forward_[0] && predicate(prev->forward_[0]->key_) == 0) {
//     auto current = prev->forward_[0];
//     auto begin_iter = SkipListIterator(current);
//     while (current->forward_[0] && predicate(current->forward_[0]->key_) ==
//     0) {
//       current = current->forward_[0];
//     }
//     auto end_iter = SkipListIterator(current->forward_[0]);
//     return std::make_pair(begin_iter, end_iter);
//   }
//   return std::nullopt;
// }

// 2种方法
std::optional<std::pair<SkipListIterator, SkipListIterator>>
SkipList::iters_monotony_predicate(
    std::function<int(const std::string &)> predicate) {
  auto prev = head;
  //检索第一个符合条件的key
  for (int i = current_level - 1; i >= 0; --i) {
    while (prev->forward_[i] && predicate(prev->forward_[i]->entry.key) > 0) {
      prev = prev->forward_[i];
    }
    if (prev->forward_[i] && predicate(prev->forward_[i]->entry.key) == 0) {
      prev = prev->forward_[i];
      break;
    }
  }
  if (prev==head) {
     return std::nullopt;
  }  
  //在底层向前后遍历
  if (prev && predicate(prev->entry.key) == 0) {
    auto end_iter = prev;
    auto begin_iter = prev;
    while (end_iter->forward_[0] &&
           predicate(end_iter->forward_[0]->entry.key) == 0) {
      end_iter = end_iter->forward_[0];
    }
    end_iter = end_iter->forward_[0];
  while (!begin_iter->backward_[0].expired()) {
     auto prev_node = begin_iter->backward_[0].lock();
     if (prev_node == head || predicate(prev_node->entry.key) != 0) {
            break;
     }
    begin_iter = prev_node;
}

    return std::make_pair(SkipListIterator(begin_iter),
                          SkipListIterator(end_iter));
  }
  return std::nullopt;
}
// ? 打印跳表, 你可以在出错时调用此函数进行调试
void SkipList::print_skiplist() {
  for (int level = 0; level < current_level; level++) {
    std::cout << "Level " << level << ": ";
    auto current = head->forward_[level];
    while (current) {
      std::cout << current->entry.key;
      current = current->forward_[level];
      if (current) {
        std::cout << " -> ";
      }
    }
    std::cout << std::endl;
  }
  std::cout << std::endl;
}
} // namespace tiny_lsm
