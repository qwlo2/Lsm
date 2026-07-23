#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace tiny_lsm {

enum class EntryState : uint8_t {
    COMMITTED = 0,
    UNCOMMITTED = 1,
    UNCOMMITTED_DEAD = 2,
};

struct SharedEntryState {
    explicit SharedEntryState(
        EntryState initial = EntryState::UNCOMMITTED)
        : state(initial) {}

    std::atomic<EntryState> state;
};

enum class ReadVisibility : uint8_t {
    COMMITTED_ONLY = 0,
    INCLUDE_UNCOMMITTED = 1,
};

enum class EntryTypr:uint8_t {
    Put=0,
    Delete=1 
};
//冒号后面代表变量类型
struct Entry {
    std::string key;
    std::string value;
    uint64_t tranc_id = 0;
    EntryState state = EntryState::COMMITTED;
    std::shared_ptr<SharedEntryState> shared_state;

    // 先预留，暂时也可以不用真正写入磁盘
    //EntryType type = EntryType::Put;
   // uint64_t commit_ts
   // TxnStatus status;     Running / Committed / Aborted
    // uint64_t start_ts;     事务开始时间，也就是读快照时间
    Entry(Entry&&e) noexcept =default;
     Entry()=default;
    Entry(const std::string& k,const std::string& v,const uint64_t& t,
          EntryState entry_state = EntryState::COMMITTED,
          std::shared_ptr<SharedEntryState> txn_state = nullptr):
    key(k),value(v),tranc_id(t),state(entry_state),
    shared_state(std::move(txn_state)){}

    EntryState effective_state() const {
        if (shared_state) {
            return shared_state->state.load(std::memory_order_acquire);
        }
        return state;
    }
     //后续把txn改为commit——ts
    bool operator==(const Entry& other)const{
           return key==other.key&&value==other.value&&tranc_id==other.tranc_id;
          
    }
    bool operator>(const Entry& other)const{
         if (key==other.key) {
             return tranc_id<other.tranc_id;
         }
         return key>other.key;
    }
    bool operator<(const Entry& other)const{
         if (key==other.key) {
             return tranc_id>other.tranc_id;
         }
         return key<other.key;
    }
};

inline bool is_state_visible(EntryState state,
                             ReadVisibility visibility) {
    if (state == EntryState::UNCOMMITTED_DEAD) {
        return false;
    }

    if (visibility == ReadVisibility::COMMITTED_ONLY) {
        return state == EntryState::COMMITTED;
    }

    return state == EntryState::COMMITTED ||
           state == EntryState::UNCOMMITTED;
}

inline bool is_state_visible(const Entry& entry,
                             ReadVisibility visibility) {
    return is_state_visible(entry.effective_state(), visibility);
}

inline bool is_delete(const Entry& entry) {
    // 当前实验可以先兼容旧逻辑
    return  entry.value.empty();
    //return  entry.eype==EntryTypr::Delete;
}

inline bool is_visible(const Entry& a, const Entry& b) {
    // 当前实验仍然用 tranc_id 作为版本上界
    return  b.tranc_id == 0 || a.tranc_id <= b.tranc_id;
    //return entryf.commit_ts<=entrys.commit_ts;
}

inline bool newer_than(const Entry& a, const Entry& b) {
    return a.tranc_id > b.tranc_id;
    //return a.commit_ts>=b.commit_ts;
}

} // namespace tiny_lsm
