#include "redis_wrapper/redis_wrapper.h"
#include "config/config.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace tiny_lsm {

// Helper functions
RedisWrapper::RedisWrapper(const std::string &db_path) {
  this->lsm = std::make_unique<LSM>(db_path);
}

size_t RedisWrapper::key_lock_index(const std::string &key) const {
  return std::hash<std::string>{}(key) % kKeyLockShardCount;
}

std::shared_mutex &RedisWrapper::key_mutex(const std::string &key) const {
  return key_lock_shards_[key_lock_index(key)];
}
//将filed_value转为filed
std::vector<std::string>
get_fileds_from_hash_value(const std::optional<std::string> &field_list_opt) {
   //value_or,有值则取，否则是参数
  std::string field_list = field_list_opt.value_or("");
  if (!field_list.empty()) {
    // 去除前缀后才是字段列表
    std::string preffix = TomlConfig::getInstance().getRedisHashValuePreffix();
    field_list =
        field_list.substr(preffix.size(), field_list.size() - preffix.size());
  }
  std::vector<std::string> fields;
  //io，读写，从str读，写入文件等（input）；从文件读，写入str，
  std::istringstream iss(field_list);
  std::string token;
  //getline，读取一句，delim是终止符，跳过
  while (std::getline(iss, token,
                      TomlConfig::getInstance().getRedisFieldSeparator())) {
    fields.push_back(token);
  }
  return fields;
}
//将filed转为key->filed_value
std::string get_hash_value_from_fields(const std::vector<std::string> &fields) {
  std::ostringstream oss;
  oss << TomlConfig::getInstance().getRedisHashValuePreffix();
  for (size_t i = 0; i < fields.size(); ++i) {
    if (i > 0)
      oss << TomlConfig::getInstance().getRedisFieldSeparator();
    oss << fields[i];
  }
  return oss.str();
}
//写入filed->value
inline std::string get_hash_filed_key(const std::string &key,
                                      const std::string &field) {
  return TomlConfig::getInstance().getRedisFieldPrefix() + key + "_" + field;
}
//hash——size
inline std::string get_hash_size_key(const std::string& key) {
  return "REDIS_HASH_SIZE_" + key;
}
//新的hash——filed
inline std::string get_hash_field_prefix(const std::string& key) {
  return TomlConfig::getInstance().getRedisFieldPrefix() + key + "_";
}
inline bool is_value_hash(const std::string &key) {
  return key.find(TomlConfig::getInstance().getRedisHashValuePreffix()) == 0;
}
//TTL，expire
inline std::string get_explire_key(const std::string &key) {
  return TomlConfig::getInstance().getRedisExpireHeader() + key;
}

// std::string get_zset_key_socre(const std::string &key,
//                                const std::string &score) {
//   std::ostringstream oss;
//   oss << std::setw(TomlConfig::getInstance().getRedisSortedSetScoreLen())
//       << std::setfill('0') << score;

//   std::string formatted_score = oss.str();

//   std::string res = TomlConfig::getInstance().getRedisSortedSetPrefix() + key +
//                     "_SCORE_" + formatted_score;
//   return res;
// }
//原实验对于同score，没有区分
std::string get_zset_key_socre(const std::string &key,
                               const std::string &score,const std::string &elem) {
  std::ostringstream oss;
  oss << std::setw(TomlConfig::getInstance().getRedisSortedSetScoreLen())
      << std::setfill('0') << score;

  std::string formatted_score = oss.str();

  std::string res = TomlConfig::getInstance().getRedisSortedSetPrefix() + key +
                    "_SCORE_" + formatted_score+"_"+elem;
  return res;
}

inline std::string get_zset_key_elem(const std::string &key,
                                     const std::string &elem) {
  return TomlConfig::getInstance().getRedisSortedSetPrefix() + key + "_ELEM_" +
         elem;
}
//带分割符是为了区分
// REDIS_SET_a...
// REDIS_SET_ab...

inline std::string get_zset_key_preffix(const std::string &key) {
  return TomlConfig::getInstance().getRedisSortedSetPrefix() + key+"_";
}

inline std::string get_zset_score_preffix(const std::string &key) {
  return TomlConfig::getInstance().getRedisSortedSetPrefix() + key + "_SCORE_";
}

inline std::string get_zset_elem_preffix(const std::string &key) {
  return TomlConfig::getInstance().getRedisSortedSetPrefix() + key + "_ELEM_";
}
//zsocre
inline std::string get_zset_score_item(const std::string &key) {
  // 定义 _SCORE_ 的前缀
  const std::string score_prefix = "_SCORE_";

  // 找到 _SCORE_ 的位置
  size_t score_pos = key.find(score_prefix);
  //size_t elem_pos=key.find("_");
  // 如果找到了 _SCORE_，则返回其右边的部分；否则返回空字符串
  if (score_pos != std::string::npos) {
    return key.substr(score_pos + score_prefix.size(),TomlConfig::getInstance().getRedisSortedSetScoreLen());
  } else {
    return "";
  }
}

inline std::string get_set_key_preffix(const std::string &key) {
  return TomlConfig::getInstance().getRedisSetPrefix() + key + "_";
 // return TomlConfig::getInstance().getRedisSetPrefix() + key;
}

inline std::string get_set_member_key(const std::string &key,
                                      const std::string &elem) {
  return TomlConfig::getInstance().getRedisSetPrefix() + key + "_" + elem;
}


inline std::string get_set_member_value() { return "1"; }

inline std::string get_set_member_prefix(const std::string &key) {
  return TomlConfig::getInstance().getRedisSetPrefix() + key + "_";
}
//expire_str是传入的超时时间，now_time是现在的时间
//在这里进行了判断，还可以相减得到ttl
bool is_expired(const std::optional<std::string> &expire_str,
                std::time_t *now_time) {
  if (!expire_str.has_value()) {
    return false;
  }
  auto now = std::chrono::system_clock::now();
  auto now_time_t = std::chrono::system_clock::to_time_t(now);

  if (now_time != nullptr) {
    *now_time = now_time_t;
  }

  // 检查是否过期
  return (std::stoll(expire_str.value()) < now_time_t);
}

std::string get_expire_time(const std::string &seconds_count) {
  // 获取当前时间戳, 以秒为单位
  auto now = std::chrono::system_clock::now();
  auto now_time_t = std::chrono::system_clock::to_time_t(now);

  auto time_add = std::stoll(seconds_count);

  // 将时间戳转换为字符串
  std::string expire_time_str = std::to_string(now_time_t + time_add);
  return expire_time_str;
}
//实验采用key-a#b#c
std::vector<std::string> split(const std::string &str, char delimiter) {
  std::vector<std::string> tokens;
  //通过输入流函数，用getline做切割
  std::istringstream iss(str);
  std::string token;
  while (std::getline(iss, token, delimiter)) {
    tokens.push_back(token);
  }
  return tokens;
}

std::string join(const std::vector<std::string> &elements, char delimiter) {
  std::ostringstream oss;
  for (size_t i = 0; i < elements.size(); ++i) {
    if (i > 0) {
      oss << delimiter;
    }
    oss << elements[i];
  }
  return oss.str();


}
//key->min_max 
inline std::pair<std::string,std::string> get_list_key_seq(const std::string &key) {
     auto pos=key.find("_");
     return  {key.substr(0,pos),key.substr(pos+1)};
}
inline std::string get_list_key_value(const std::string &start,const std::string& stop) {
  return start+"_"+stop;
}
inline std::string get_list_value_key(const std::string &key,const uint32_t &seq) {
  std::ostringstream os;
  os<<std::setw(32)<<std::setfill('0')<<std::to_string(seq);
  return "REDIS_LIST_PREFIX_"+ key + "_" + os.str();
}
//本来是不加_，谓词查询就不会查到这个，但是_要用来区分abc，b，因此在谓词for中跳过
inline std::string get_list_key_prefix(const std::string &key) {
  return "REDIS_LIST_PREFIX_"+ key + "_" ;
}
//元信息，方便del
inline std::string get_type_key(const std::string &key) {
  return "REDIS_TYPE_" + key;
}
constexpr const char *REDIS_TYPE_STRING = "string";
constexpr const char *REDIS_TYPE_HASH = "hash";
constexpr const char *REDIS_TYPE_SET = "set";
constexpr const char *REDIS_TYPE_ZSET = "zset";
constexpr const char *REDIS_TYPE_LIST = "list";

constexpr const char *REDIS_WRONGTYPE =
    "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n";

// ************************ Redis *************************
std::optional<std::string>
RedisWrapper::get_type_cached(const std::string &key) {
  {
    std::shared_lock lock(type_cache_mtx_);
    auto cache_it = type_cache_.find(key);
    if (cache_it != type_cache_.end()) {
      return cache_it->second;
    }
  }

  auto type = lsm->get(get_type_key(key));
  if (type) {
    std::unique_lock lock(type_cache_mtx_);
    type_cache_[key] = *type;
  }
  return type;
}

// 调用者必须持有逻辑 key 的独占锁。
bool RedisWrapper::check_or_create_type_locked(const std::string &key,
                                               const std::string &type) {
  auto old_type = get_type_cached(key);
  if (old_type) {
    return *old_type == type;
  }

  lsm->put(get_type_key(key), type);
  {
    std::unique_lock lock(type_cache_mtx_);
    type_cache_[key] = type;
  }
  return true;
}

// 调用者必须已经独占持有全部四把缓存锁。
void RedisWrapper::erase_key_cache_unlocked(const std::string &key) {
  type_cache_.erase(key);
  set_size_cache_.erase(key);
  set_member_cache_.erase(key);
  set_member_loaded_.erase(key);
  hash_field_cache_.erase(key);
  hash_size_cache_.erase(key);
  hash_loaded_.erase(key);
  zset_size_cache_.erase(key);
  zset_elem_score_cache_.erase(key);
  zset_loaded_.erase(key);
}

void RedisWrapper::erase_key_cache(const std::string &key) {
  std::scoped_lock lock(type_cache_mtx_, hash_cache_mtx_, set_cache_mtx_,
                        zset_cache_mtx_);
  erase_key_cache_unlocked(key);
}

// 调用者必须持有逻辑 key 的共享锁或独占锁。
void RedisWrapper::load_set_cache_locked(const std::string &key) {
  {
    std::shared_lock lock(set_cache_mtx_);
    if (set_member_loaded_.contains(key)) {
      return;
    }
  }

  auto prefix = get_set_member_prefix(key);
  auto size_key = get_set_key_preffix(key);
  std::unordered_set<std::string> members;
  auto result = lsm->lsm_iters_monotony_predicate(
      0, [&prefix](const std::string &k) {
        return -k.compare(0, prefix.size(), prefix);
      });

  if (result) {
    auto begin = result->first;
    auto end = result->second;
    for (; begin != end && begin.is_valid(); ++begin) {
      const auto &full_key = (*begin).first;
      if (full_key == size_key || full_key.size() < prefix.size() ||
          full_key.compare(0, prefix.size(), prefix) != 0) {
        continue;
      }
      members.emplace(full_key.substr(prefix.size()));
    }
  }

  std::unique_lock lock(set_cache_mtx_);
  if (!set_member_loaded_.contains(key)) {
    set_size_cache_[key] = static_cast<int64_t>(members.size());
    set_member_cache_[key] = std::move(members);
    set_member_loaded_.emplace(key);
  }
}

// 调用者必须持有逻辑 key 的共享锁或独占锁。
void RedisWrapper::load_hash_cache_locked(const std::string &key) {
  {
    std::shared_lock lock(hash_cache_mtx_);
    if (hash_loaded_.contains(key)) {
      return;
    }
  }

  auto field_prefix = get_hash_field_prefix(key);
  std::unordered_set<std::string> fields;
  auto result = lsm->lsm_iters_monotony_predicate(
      0, [&field_prefix](const std::string &k) {
        return -k.compare(0, field_prefix.size(), field_prefix);
      });

  if (result) {
    auto begin = result->first;
    auto end = result->second;
    for (; begin != end && begin.is_valid(); ++begin) {
      const auto &full_key = (*begin).first;
      if (full_key.size() < field_prefix.size() ||
          full_key.compare(0, field_prefix.size(), field_prefix) != 0) {
        continue;
      }
      fields.emplace(full_key.substr(field_prefix.size()));
    }
  }

  std::unique_lock lock(hash_cache_mtx_);
  if (!hash_loaded_.contains(key)) {
    hash_size_cache_[key] = static_cast<int64_t>(fields.size());
    hash_field_cache_[key] = std::move(fields);
    hash_loaded_.emplace(key);
  }
}

// 调用者必须持有逻辑 key 的共享锁或独占锁。
void RedisWrapper::load_zset_cache_locked(const std::string &key) {
  {
    std::shared_lock lock(zset_cache_mtx_);
    if (zset_loaded_.contains(key)) {
      return;
    }
  }

  auto elem_prefix = get_zset_elem_preffix(key);
  std::unordered_map<std::string, std::string> elem_scores;
  auto result = lsm->lsm_iters_monotony_predicate(
      0, [&elem_prefix](const std::string &k) {
        return -k.compare(0, elem_prefix.size(), elem_prefix);
      });

  if (result) {
    auto begin = result->first;
    auto end = result->second;
    for (; begin != end && begin.is_valid(); ++begin) {
      auto item = *begin;
      if (item.first.size() < elem_prefix.size() ||
          item.first.compare(0, elem_prefix.size(), elem_prefix) != 0 ||
          item.first.empty() || item.second.empty()) {
        continue;
      }
      elem_scores.emplace(item.first.substr(elem_prefix.size()), item.second);
    }
  }

  std::unique_lock lock(zset_cache_mtx_);
  if (!zset_loaded_.contains(key)) {
    zset_size_cache_[key] = static_cast<int64_t>(elem_scores.size());
    zset_elem_score_cache_[key] = std::move(elem_scores);
    zset_loaded_.emplace(key);
  }
}

// 调用者必须持有逻辑 key 的独占锁。
bool RedisWrapper::delete_key_locked(const std::string &key) {
  auto type = get_type_cached(key);
  bool existed = false;
  std::vector<std::string> remove_keys;
  //通过前缀收集所有要删除的，hash要额外加key->faiel
  auto collect_prefix_keys = [&](const std::string &prefix) {
    auto result = lsm->lsm_iters_monotony_predicate(
        0, [&prefix](const std::string &k) {
          return -k.compare(0, prefix.size(), prefix);
        });

    if (result) {
      auto [begin, end] = result.value();
      for (; begin != end&& begin.is_valid(); ++begin) {
        remove_keys.emplace_back((*begin).first);
      }
    }
  };
  if (type) {
     existed=true;
    // switch只能用于枚举或整数
     if (type.value() == REDIS_TYPE_STRING) {
      remove_keys.emplace_back(key);
    } else if (type.value() == REDIS_TYPE_HASH) {
      // auto fields = get_fileds_from_hash_value(lsm->get(key));
      // for (auto &field : fields) {
      //   remove_keys.emplace_back(get_hash_filed_key(key, field));
      // }
      // remove_keys.emplace_back(key);
      collect_prefix_keys(get_hash_field_prefix(key));
      remove_keys.emplace_back(get_hash_size_key(key));
    } else if (type.value() == REDIS_TYPE_SET) {
      collect_prefix_keys(get_set_key_preffix(key));
     // remove_keys.emplace_back(get_set_key_preffix(key));
    } else if (type.value() == REDIS_TYPE_ZSET) {
      collect_prefix_keys(get_zset_key_preffix(key));
    } else if (type.value() == REDIS_TYPE_LIST) {
      collect_prefix_keys(get_list_key_prefix(key));
    }

    remove_keys.emplace_back(get_type_key(key));
    remove_keys.emplace_back(get_explire_key(key));
  } 
  if (!remove_keys.empty()) {
    lsm->remove_batch(remove_keys);
  }
  erase_key_cache(key);
  return existed;
}

bool RedisWrapper::expire_clean_locked(const std::string &key) {
  if (!is_expired(lsm->get(get_explire_key(key)), nullptr)) {
    return false;
  }
  delete_key_locked(key);
  return true;
}

// 读命令的共享 key 锁升级。升级期间必须二次确认过期状态，
// 防止另一个写命令刚好重建了同名 key。
bool RedisWrapper::expire_clean(
    const std::string &key,
    std::shared_lock<std::shared_mutex> &rlock) {
  if (!is_expired(lsm->get(get_explire_key(key)), nullptr)) {
    return false;
  }

  rlock.unlock();
  std::unique_lock wlock(key_mutex(key));
  if (is_expired(lsm->get(get_explire_key(key)), nullptr)) {
    delete_key_locked(key);
    return true;
  }

  wlock.unlock();
  rlock = std::shared_lock<std::shared_mutex>(key_mutex(key));
  return false;
}

// ************************* Redis Command *************************
// 基础操作
std::string RedisWrapper::set(std::vector<std::string> &args) {
  return redis_set(args[1], args[2]);
}
std::string RedisWrapper::get(std::vector<std::string> &args) {
  return redis_get(args[1]);
}
std::string RedisWrapper::del(std::vector<std::string> &args) {
  return redis_del(args);
}
std::string RedisWrapper::incr(std::vector<std::string> &args) {
  return redis_incr(args[1]);
}

std::string RedisWrapper::decr(std::vector<std::string> &args) {
  return redis_decr(args[1]);
}

std::string RedisWrapper::expire(std::vector<std::string> &args) {
  return redis_expire(args[1], args[2]);
}

std::string RedisWrapper::ttl(std::vector<std::string> &args) {
  return redis_ttl(args[1]);
}

// 哈希操作
std::string RedisWrapper::hset(std::vector<std::string> &args) {
  // return redis_hset(args[1], args[2], args[3]);
  if (args.size() < 4 || (args.size() - 1) % 2 != 1) {
    return "-ERR wrong number of arguments for 'hset' command\r\n";
  }

  const std::string &key = args[1];
  std::vector<std::pair<std::string, std::string>> fieldValues;

  // 从第2个参数开始，每两个为一组 field-value
  for (size_t i = 2; i < args.size(); i += 2) {
    if (i + 1 >= args.size())
      break; // 防止越界
    fieldValues.emplace_back(args[i], args[i + 1]);
  }
  return redis_hset_batch(key, fieldValues);
}

std::string RedisWrapper::hget(std::vector<std::string> &args) {
  return redis_hget(args[1], args[2]);
}

std::string RedisWrapper::hdel(std::vector<std::string> &args) {
  return redis_hdel(args[1], args[2]);
}

std::string RedisWrapper::hkeys(std::vector<std::string> &args) {
  return redis_hkeys(args[1]);
}

// 链表操作
std::string RedisWrapper::lpush(std::vector<std::string> &args) {
  return redis_lpush(args[1], args[2]);
}
std::string RedisWrapper::rpush(std::vector<std::string> &args) {
  return redis_rpush(args[1], args[2]);
}
std::string RedisWrapper::lpop(std::vector<std::string> &args) {
  return redis_lpop(args[1]);
}
std::string RedisWrapper::rpop(std::vector<std::string> &args) {
  return redis_rpop(args[1]);
}
std::string RedisWrapper::llen(std::vector<std::string> &args) {
  return redis_llen(args[1]);
}
std::string RedisWrapper::lrange(std::vector<std::string> &args) {
  int start = std::stoi(args[2]);
  int end = std::stoi(args[3]);

  return redis_lrange(args[1], start, end);
}

// 有序集合操作
std::string RedisWrapper::zadd(std::vector<std::string> &args) {
  return redis_zadd(args);
}

std::string RedisWrapper::zrem(std::vector<std::string> &args) {
  return redis_zrem(args);
}

std::string RedisWrapper::zrange(std::vector<std::string> &args) {
  return redis_zrange(args);
}

std::string RedisWrapper::zcard(std::vector<std::string> &args) {

  return redis_zcard(args[1]);
}

std::string RedisWrapper::zscore(std::vector<std::string> &args) {
  return redis_zscore(args[1], args[2]);
}
std::string RedisWrapper::zincrby(std::vector<std::string> &args) {
  return redis_zincrby(args[1], args[2], args[3]);
}

std::string RedisWrapper::zrank(std::vector<std::string> &args) {
  return redis_zrank(args[1], args[2]);
}

// 无序集合操作
std::string RedisWrapper::sadd(std::vector<std::string> &args) {
  return redis_sadd(args);
}

std::string RedisWrapper::srem(std::vector<std::string> &args) {
  return redis_srem(args);
}

std::string RedisWrapper::sismember(std::vector<std::string> &args) {
  return redis_sismember(args[1], args[2]);
}

std::string RedisWrapper::scard(std::vector<std::string> &args) {
  return redis_scard(args[1]);
}

std::string RedisWrapper::smembers(std::vector<std::string> &args) {
  return redis_smembers(args[1]);
}
 std::string RedisWrapper::hsetnx(std::vector<std::string> &args){
     return  hset_not_exitst(args);
 }
void RedisWrapper::clear() {
  std::vector<std::unique_lock<std::shared_mutex>> key_locks;
  key_locks.reserve(kKeyLockShardCount);
  for (auto &shard : key_lock_shards_) {
    key_locks.emplace_back(shard);
  }

  {
    std::scoped_lock cache_locks(type_cache_mtx_, hash_cache_mtx_,
                                 set_cache_mtx_, zset_cache_mtx_);
    type_cache_.clear();
    set_size_cache_.clear();
    set_member_cache_.clear();
    set_member_loaded_.clear();
    hash_field_cache_.clear();
    hash_size_cache_.clear();
    hash_loaded_.clear();
    zset_size_cache_.clear();
    zset_elem_score_cache_.clear();
    zset_loaded_.clear();
  }

  lsm->clear();
}
void RedisWrapper::flushall() { this->lsm->flush(); }

// *********************** Redis ***********************
// 基础操作
//存在值类型则自增，否则新建-1，1，非值则-ERR
std::string RedisWrapper::redis_incr(const std::string &key) {
  std::unique_lock key_lock(key_mutex(key));
  expire_clean_locked(key);

  auto type = get_type_cached(key);
  if (type && *type != REDIS_TYPE_STRING) {
    return REDIS_WRONGTYPE;
  }

  if (auto value_query = lsm->get(key)) {
    int value = 0;
    auto [ptr, ec] = std::from_chars(value_query->data(),
                                     value_query->data() + value_query->size(),
                                     value);
    if (ptr != value_query->data() + value_query->size() ||
        ec != std::errc()) {
      return "-ERR the value is not an integer\r\n";
    }
    ++value;
    lsm->put(key, std::to_string(value));
    return std::to_string(value);
  }

  lsm->put_batch(
      {{key, "1"}, {get_type_key(key), REDIS_TYPE_STRING}});
  {
    std::unique_lock cache_lock(type_cache_mtx_);
    type_cache_[key] = REDIS_TYPE_STRING;
  }
  return "1";
}
//Redis 批量删除是内部按 key 一个个处理，但放在一条命令里执行
std::string RedisWrapper::redis_del(std::vector<std::string> &args) {
  std::vector<size_t> shard_indices;
  shard_indices.reserve(args.size() > 1 ? args.size() - 1 : 0);
  for (size_t i = 1; i < args.size(); ++i) {
    shard_indices.emplace_back(key_lock_index(args[i]));
  }
  std::sort(shard_indices.begin(), shard_indices.end());
  shard_indices.erase(
      std::unique(shard_indices.begin(), shard_indices.end()),
      shard_indices.end());

  std::vector<std::unique_lock<std::shared_mutex>> key_locks;
  key_locks.reserve(shard_indices.size());
  for (size_t shard_index : shard_indices) {
    key_locks.emplace_back(key_lock_shards_[shard_index]);
  }

  int del_count = 0;
  for (size_t i = 1; i < args.size(); ++i) {
    if (delete_key_locked(args[i])) {
      ++del_count;
    }
  }

  return ":" + std::to_string(del_count) + "\r\n";
   
}
std::string RedisWrapper::redis_decr(const std::string &key) {
  std::unique_lock key_lock(key_mutex(key));
  expire_clean_locked(key);

  auto type = get_type_cached(key);
  if (type && *type != REDIS_TYPE_STRING) {
    return REDIS_WRONGTYPE;
  }

  if (auto value_query = lsm->get(key)) {
    int value = 0;
    auto [ptr, ec] = std::from_chars(value_query->data(),
                                     value_query->data() + value_query->size(),
                                     value);
    if (ptr != value_query->data() + value_query->size() ||
        ec != std::errc()) {
      return "-ERR the value is not an integer\r\n";
    }
    --value;
    lsm->put(key, std::to_string(value));
    return std::to_string(value);
  }

  lsm->put_batch(
      {{key, "-1"}, {get_type_key(key), REDIS_TYPE_STRING}});
  {
    std::unique_lock cache_lock(type_cache_mtx_);
    type_cache_[key] = REDIS_TYPE_STRING;
  }
  return "-1";
}
std::string RedisWrapper::redis_expire(const std::string &key,
                                       std::string seconds_count) {
  std::unique_lock key_lock(key_mutex(key));
  if (expire_clean_locked(key) || !get_type_cached(key)) {
    return ":0\r\n";
  }

  lsm->put(get_explire_key(key), get_expire_time(seconds_count));
  return ":1\r\n";
}
std::string RedisWrapper::redis_set(std::string &key, std::string &value) {
  std::unique_lock key_lock(key_mutex(key));
  lsm->put_batch({
      {key, value},
      {get_type_key(key), REDIS_TYPE_STRING},
  });
  erase_key_cache(key);
  {
    std::unique_lock cache_lock(type_cache_mtx_);
    type_cache_[key] = REDIS_TYPE_STRING;
  }
  return "+OK\r\n";
}
std::string RedisWrapper::redis_get(std::string &key) {
  std::shared_lock<std::shared_mutex> key_lock(key_mutex(key));
  if (expire_clean(key, key_lock)) {
    return "$-1\r\n";
  }

  auto type = get_type_cached(key);
  if (!type) {
  return "$-1\r\n";
}
  if (type && *type != REDIS_TYPE_STRING) {
    return REDIS_WRONGTYPE;
  }
  if (auto value = lsm->get(key)) {
    return "$" + std::to_string(value->size()) + "\r\n" + *value + "\r\n";
  }
  return "$-1\r\n"; // 表示键不存在
}
//del，ttl，expire要兼容不同类型，暂时先不改
std::string RedisWrapper::redis_ttl(std::string &key) {
  std::shared_lock<std::shared_mutex> key_lock(key_mutex(key));
  if (!get_type_cached(key)) {
    return ":-2\r\n";
  }

  if (auto expire_query = lsm->get(get_explire_key(key))) {
    time_t now_time{};
    if (is_expired(expire_query, &now_time)) {
      return ":-2\r\n";
    }
    return ":" + std::to_string(std::stoll(*expire_query) - now_time) +
           "\r\n";
  }
  return ":-1\r\n";
}

// 哈希操作
//要返回增加数量
std::string RedisWrapper::redis_hset_batch(
    const std::string &key,
   const std::vector<std::pair<std::string, std::string>> &field_value_pairs) {
  std::unique_lock key_lock(key_mutex(key));
  expire_clean_locked(key);
  if (!check_or_create_type_locked(key, REDIS_TYPE_HASH)) {
    return REDIS_WRONGTYPE;
  }
  load_hash_cache_locked(key);

  std::unordered_set<std::string> new_fields;
  int64_t current_size = 0;
  {
    std::shared_lock cache_lock(hash_cache_mtx_);
    const auto &fields = hash_field_cache_.at(key);
    current_size = hash_size_cache_.at(key);
    for (const auto &[field, value] : field_value_pairs) {
      (void)value;
      if (!fields.contains(field)) {
        new_fields.emplace(field);
      }
    }
  }

  const int added = static_cast<int>(new_fields.size());
  const int64_t new_size = current_size + added;
  std::vector<std::pair<std::string, std::string>> puts;
  puts.reserve(field_value_pairs.size() + (added > 0 ? 1 : 0));
  for (const auto &[field, value] : field_value_pairs) {
    puts.emplace_back(get_hash_filed_key(key, field), value);
  }
  if (added > 0) {
    puts.emplace_back(get_hash_size_key(key), std::to_string(new_size));
  }
  if (!puts.empty()) {
    lsm->put_batch(puts);
  }

  {
    std::unique_lock cache_lock(hash_cache_mtx_);
    auto &fields = hash_field_cache_[key];
    for (const auto &field : new_fields) {
      fields.emplace(field);
    }
    hash_size_cache_[key] = new_size;
  }
  return ":" + std::to_string(added) + "\r\n";
}
//filed是动态增加
std::string RedisWrapper::redis_hset(const std::string &key,
                                     const std::string &field,
                                     const std::string &value) {
    
  //  std::shared_lock rlock(redis_mtx);
  //  //是否有旧的key存在且过期
  //   if (!expire_clean(key,rlock)) {
  //       rlock.unlock();
  //   }
  //   std::unique_lock<std::shared_mutex> wlock(redis_mtx);
  //      //上面判断是否过期，并不影响判断是否同命名空间
  //     if (!check_or_create_type_unlocked(key,REDIS_TYPE_HASH)) {
  //        return  REDIS_WRONGTYPE;
  //     }
  //      auto hash_key=lsm->get(key);
  //  //存在则检测有无filed
  //  if (hash_key) {
  //      //auto& filed_value=hash_key.value();
  //      //新filed,默认从0开始，没有找到返回npos
  //      //没有用str——find，是因为会出现abc中的b
  //       auto filed_value=std::move(get_fileds_from_hash_value(hash_key));
  //      if (find(filed_value.begin(), filed_value.end(), field)==filed_value.end()) {
            
  //          // auto field_list=std::move(get_fileds_from_hash_value(filed_value));
  //          filed_value.emplace_back(field);
  //           auto key_hash_value=std::move(get_hash_value_from_fields(filed_value));
           
  //          //std::unique_lock wlock(redis_mtx);
  //           //将新key_hash_value写入
  //           lsm->put_batch({{key,key_hash_value},{get_hash_filed_key(key,field),value}});
  //           return "+ OK\r\n";
  //      }
  //      //久,直接put
  //     // rlock.unlock();
  //      // std::unique_lock wlock(redis_mtx);
  //      lsm->put(get_hash_filed_key(key,field),value);
  //       return "+ OK\r\n";
  //  }else {
  //      //auto key_hash_value=std::move(get_hash_value_from_fields({field}));
  //      auto key_hash_value=TomlConfig::getInstance().getRedisHashValuePreffix()+"$"+field;
  //       lsm->put_batch({{key,key_hash_value},{get_hash_filed_key(key,field),value}});
  //       return "+ OK\r\n";
  //  }
return redis_hset_batch(key,{std::make_pair(field, value)});
}  

std::string RedisWrapper::redis_hget(const std::string &key,
                                     const std::string &field) {
  std::shared_lock<std::shared_mutex> key_lock(key_mutex(key));
  if (expire_clean(key, key_lock)) {
    return "$-1\r\n";
  }
  auto type = get_type_cached(key);
  if (type && *type != REDIS_TYPE_HASH) {
    return REDIS_WRONGTYPE;
  }
  if (auto value = lsm->get(get_hash_filed_key(key, field))) {
    return "$" + std::to_string(value->size()) + "\r\n" + *value + "\r\n";
  }
  return "$-1\r\n";
}

std::string RedisWrapper::redis_hdel(const std::string &key,
                                     const std::string &field) {
  std::unique_lock key_lock(key_mutex(key));
  if (expire_clean_locked(key)) {
    return ":0\r\n";
  }
  auto type = get_type_cached(key);
  if (type && *type != REDIS_TYPE_HASH) {
    return REDIS_WRONGTYPE;
  }
  if (!type) {
    return ":0\r\n";
  }
  load_hash_cache_locked(key);

  int64_t new_size = 0;
  {
    std::shared_lock cache_lock(hash_cache_mtx_);
    const auto &fields = hash_field_cache_.at(key);
    if (!fields.contains(field)) {
      return ":0\r\n";
    }
    new_size = hash_size_cache_.at(key) - 1;
  }

  std::vector<std::string> removes;
  removes.emplace_back(get_hash_filed_key(key, field));
  if (new_size == 0) {
    removes.emplace_back(get_hash_size_key(key));
    removes.emplace_back(get_type_key(key));
    removes.emplace_back(get_explire_key(key));
  } else {
    lsm->put(get_hash_size_key(key), std::to_string(new_size));
  }
  lsm->remove_batch(removes);
  if (new_size == 0) {
    erase_key_cache(key);
  } else {
    std::unique_lock cache_lock(hash_cache_mtx_);
    hash_field_cache_[key].erase(field);
    hash_size_cache_[key] = new_size;
  }
  return ":1\r\n";
}
//返回所有failed
std::string RedisWrapper::redis_hkeys(const std::string &key) {
  std::shared_lock<std::shared_mutex> key_lock(key_mutex(key));
  if (expire_clean(key, key_lock)) {
    return "*0\r\n";
  }
  auto type = get_type_cached(key);
  if (type && *type != REDIS_TYPE_HASH) {
    return REDIS_WRONGTYPE;
  }
  if (!type) {
    return "*0\r\n";
  }

  load_hash_cache_locked(key);
  std::vector<std::string> fields;
  {
    std::shared_lock cache_lock(hash_cache_mtx_);
    const auto &cached_fields = hash_field_cache_.at(key);
    fields.assign(cached_fields.begin(), cached_fields.end());
  }
  std::sort(fields.begin(), fields.end());

  std::string result = "*" + std::to_string(fields.size()) + "\r\n";
  for (const auto &field : fields) {
    result += "$" + std::to_string(field.size()) + "\r\n" + field + "\r\n";
  }
  return result;
}

// 链表操作
//返回 list 当前长度
std::string RedisWrapper::redis_lpush(const std::string &key,
                                      const std::string &value) {
    std::unique_lock key_lock(key_mutex(key));
    expire_clean_locked(key);
    if (!check_or_create_type_locked(key, REDIS_TYPE_LIST)) {
      return REDIS_WRONGTYPE;
    }
    auto list_seq=lsm->get(get_list_key_prefix(key));
    std::vector<std::pair<std::string,std::string>> tmp;
    int count=0;
    if (list_seq) {
        auto seq=get_list_key_seq(list_seq.value());
         auto start=std::stoull(seq.first);
        tmp.emplace_back(get_list_key_prefix(key),std::to_string(start-1)+"_"+seq.second);
        tmp.emplace_back(get_list_value_key(key,start-1),value);
        count=std::stoull(seq.second)-start+2;
    }else {
       //新建
       tmp.emplace_back(get_list_key_prefix(key),std::to_string(UINT32_MAX / 2)+"_"+std::to_string(UINT32_MAX / 2));
        tmp.emplace_back(get_list_value_key(key,UINT32_MAX / 2),value);
        count=1;
    }
   lsm->put_batch(tmp);
   return ":"+std::to_string(count)+"\r\n";
}

std::string RedisWrapper::redis_rpush(const std::string &key,
                                      const std::string &value) {
    std::unique_lock key_lock(key_mutex(key));
    expire_clean_locked(key);
    if (!check_or_create_type_locked(key, REDIS_TYPE_LIST)) {
      return REDIS_WRONGTYPE;
    }
    auto list_seq=lsm->get(get_list_key_prefix(key));
    std::vector<std::pair<std::string,std::string>> tmp;
    int count=0;
    if (list_seq) {
        auto seq=get_list_key_seq(list_seq.value());
         auto stop=std::stoull(seq.second);
        tmp.emplace_back(get_list_key_prefix(key),seq.first+"_"+std::to_string(stop+1));
        tmp.emplace_back(get_list_value_key(key,stop+1),value);
        count=stop-std::stoull(seq.first)+2;
    }else {
       //新建
        tmp.emplace_back(get_list_key_prefix(key),std::to_string(UINT32_MAX / 2)+"_"+std::to_string(UINT32_MAX / 2));
        tmp.emplace_back(get_list_value_key(key,UINT32_MAX / 2),value);
        count=1;
    }
   lsm->put_batch(tmp);
   return ":"+std::to_string(count)+"\r\n";
}

std::string RedisWrapper::redis_lpop(const std::string &key) {
  std::unique_lock key_lock(key_mutex(key));
  if (expire_clean_locked(key)) {
    return "$-1\r\n";
  }
  auto type = get_type_cached(key);
  if (type && *type != REDIS_TYPE_LIST) {
    return REDIS_WRONGTYPE;
  }
    auto list_seq=lsm->get(get_list_key_prefix(key));
    
    std::vector<std::pair<std::string,std::string>> tmp;
    std::string value;
    bool became_empty = false;
    // int ans;
    if (list_seq) {
        auto seq=get_list_key_seq(list_seq.value());
         auto start=std::stoull(seq.first);
        tmp.emplace_back(get_list_key_prefix(key),std::to_string(start+1)+"_"+seq.second);
        tmp.emplace_back(get_list_value_key(key,start),"");
        //被弹出元素
         value = lsm->get(get_list_value_key(key, start)).value();
        //可能需要判断
       // ans=std::stoi(seq.second)-std::stoi(seq.first)+1;
        if (std::stoull(seq.second)==std::stoull(seq.first)) {
          tmp.emplace_back(get_list_key_prefix(key),"");
           tmp.emplace_back(get_type_key(key),"");
            tmp.emplace_back(get_explire_key(key),"");
          became_empty = true;
        }
    }else {
       return "$-1\r\n";
    }
   lsm->put_batch(tmp);
   if (became_empty) {
     erase_key_cache(key);
   }
   //return ":"+std::to_string(ans-1)+"\r\n";
   return "$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
}

std::string RedisWrapper::redis_rpop(const std::string &key) {
  std::unique_lock key_lock(key_mutex(key));
  if (expire_clean_locked(key)) {
    return "$-1\r\n";
  }
  auto type = get_type_cached(key);
  if (type && *type != REDIS_TYPE_LIST) {
    return REDIS_WRONGTYPE;
  }
   
    auto list_seq=lsm->get(get_list_key_prefix(key));
    std::vector<std::pair<std::string,std::string>> tmp;
    //int ans;
    std::string value;
    bool became_empty = false;
    if (list_seq) {
        auto seq=get_list_key_seq(list_seq.value());
         auto stop=std::stoull(seq.second);
        tmp.emplace_back(get_list_key_prefix(key),seq.first+"_"+std::to_string(stop-1));
        tmp.emplace_back(get_list_value_key(key,stop),"");
         //被弹出元素
         value = lsm->get(get_list_value_key(key, stop)).value();
        //可能需要判断
       //ans=std::stoi(seq.second)-std::stoi(seq.first)+1;
        if (std::stoull(seq.second)==std::stoull(seq.first)) {
           tmp.emplace_back(get_list_key_prefix(key),"");
           tmp.emplace_back(get_type_key(key),"");
            tmp.emplace_back(get_explire_key(key),"");
           became_empty = true;
        }
    }else {
       return "$-1\r\n";
    }
   lsm->put_batch(tmp);
   if (became_empty) {
     erase_key_cache(key);
   }
   //return ":"+std::to_string(ans-1)+"\r\n";
   return "$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
}

std::string RedisWrapper::redis_llen(const std::string &key) {
    std::shared_lock<std::shared_mutex> key_lock(key_mutex(key));
    
     //是否有旧的key存在且过期
    if (expire_clean(key,key_lock)) {
          return ":0\r\n";
    }
    
    auto type = get_type_cached(key);
  if (type && *type != REDIS_TYPE_LIST) {
    return REDIS_WRONGTYPE;
  }
   
    auto list_seq=lsm->get(get_list_key_prefix(key));
   
    if (list_seq) {
             auto seq=get_list_key_seq(list_seq.value());
             return ":"+std::to_string(std::stoull(seq.second)-std::stoull(seq.first)+1)+"\r\n";
    }
    return ":0\r\n";
}

std::string RedisWrapper::redis_lrange(const std::string &key, int start,
                                       int stop) {
  std::shared_lock<std::shared_mutex> key_lock(key_mutex(key));
  if (expire_clean(key, key_lock)) {
   return "*0\r\n";
  }
  auto type = get_type_cached(key);
  if (type && *type != REDIS_TYPE_LIST) {
    return REDIS_WRONGTYPE;
  }
  if (!type) {
    return "*0\r\n";
  }
    
    auto list_seq=lsm->get(get_list_key_prefix(key));
    auto list_pre=std::move(get_list_key_prefix(key));
    auto list_quary=std::move(lsm->lsm_iters_monotony_predicate(0,[&list_pre](const std::string& key){
            return  -key.compare(0,list_pre.size(),list_pre);
    }));
  if (!list_quary) {
    return "*0\r\n";
  }
  // int count=0;
  std::vector<std::pair<std::string,std::string>> sort;
  std::string ans;
  auto begin=std::move(list_quary->first);
  auto end=std::move(list_quary->second);
  for (; begin != end; ++begin) {
    if ((*begin).first == get_list_key_prefix(key)) {
      continue;
    }
    sort.emplace_back((*begin).first, (*begin).second);
  }
  //soet为空，或者start太大
  if (sort.empty()) {
    return "*0\r\n";
  }
    //倒数+size+1,但是是数组只用加size即可
    if (start<0) {
      start+=sort.size();
    }
   if (stop<0) {
      stop += sort.size();
   }
   //倒数的个数超过了总数，即从头开始
   if (start<0) {
      start=0;
   }
   //结束位置不能小于第一个
   if (start>stop||start>=(int)sort.size()) {
      return "*0\r\n";
   }
   //防止stop越界
   stop = std::min(stop, (int)sort.size() - 1);
   int count = stop - start + 1;
   for (;start<=stop;++start) {
     ans+="$"+std::to_string(sort[start].second.size())+"\r\n"+sort[start].second+"\r\n";
   }
   //count=start-stop+1;
  //不在区间内
  if (ans=="") {
     return "*0\r\n";
  }
  return "*"+std::to_string(count)+"\r\n"+ans;
};

// std::string RedisWrapper::redis_zcard(const std::string &key) {
//      std::shared_lock<std::shared_mutex> rlock(redis_mtx);
   // 过期
//   if (expire_zset_clean(key, rlock)) {
//     return ":0\r\n";
//   }
  //无特殊情况，不会出现非过期但是无数量
//    if (auto set_quary=lsm->get(get_zset_key_preffix(key))) {
//          return ":" + set_quary.value() + "\r\n";
//    }
//     return "$-1\r\n";
// }
//socre elem...
std::string RedisWrapper::redis_zadd(std::vector<std::string> &args) {
    if ((args.size() - 2) % 2 != 0) {
      return REDIS_WRONGTYPE;
    }

    const std::string &key = args[1];
    std::unique_lock key_lock(key_mutex(key));
    expire_clean_locked(key);
    if (!check_or_create_type_locked(key, REDIS_TYPE_ZSET)) {
      return REDIS_WRONGTYPE;
    }
    load_zset_cache_locked(key);

    std::unordered_map<std::string, std::string> final_scores;
    for (size_t i = 2; i < args.size(); i += 2) {
      final_scores[args[i + 1]] = args[i];
    }

    int added = 0;
    int64_t current_size = 0;
    std::vector<std::pair<std::string, std::string>> put_z;
    {
      std::shared_lock cache_lock(zset_cache_mtx_);
      const auto &cached_scores = zset_elem_score_cache_.at(key);
      current_size = zset_size_cache_.at(key);
      for (const auto &[elem, score] : final_scores) {
        auto it = cached_scores.find(elem);
        if (it == cached_scores.end()) {
          ++added;
        } else if (it->second != score) {
          put_z.emplace_back(get_zset_key_socre(key, it->second, elem), "");
        }
      }
    }

    const int64_t new_size = current_size + added;
    for (const auto &[elem, score] : final_scores) {
      put_z.emplace_back(get_zset_key_elem(key, elem), score);
      put_z.emplace_back(get_zset_key_socre(key, score, elem), elem);
    }
    if (added > 0) {
      put_z.emplace_back(get_zset_key_preffix(key), std::to_string(new_size));
    }
    if (!put_z.empty()) {
      lsm->put_batch(put_z);
    }

    {
      std::unique_lock cache_lock(zset_cache_mtx_);
      auto &cached_scores = zset_elem_score_cache_[key];
      for (const auto &[elem, score] : final_scores) {
        cached_scores[elem] = score;
      }
      zset_size_cache_[key] = new_size;
    }
    return ":" + std::to_string(added) + "\r\n";
    //   int n=args.size();
    // std::unordered_map<std::string, std::string> new_scores;
    // new_scores.reserve((n - 2) / 2);
    //   for (int i=2;i<n; i += 2) {
    //     const auto &score = args[i];
    //     const auto &elem = args[i + 1];
    //       new_scores.emplace(elem,score);
    //     get_el.emplace_back(el_pre + elem);

    //     put_z.emplace_back(get_zset_key_socre(args[1], score, elem), elem);
    //     put_z.emplace_back(el_pre + elem, score);
    //   }
    //   auto z_quary=std::move(lsm->get_batch(get_el));
    //    n=0;
    //    //统计有多少新add
    //   for (auto& it : z_quary) {
    //     //新的计数，旧的要删除旧的score
    //    const  auto & elem = it.first.substr(el_pre.size());
    //      const  auto & ns = new_scores.find(elem);
    //       if (!it.second) {
    //         ++n;
    //       } else if (ns != new_scores.end() && it.second.value() !=
    //       ns->second) {
    //         put_z.emplace_back(
    //             get_zset_key_socre(args[1], it.second.value(), elem), "");
    //       }
    //   }
    //   if ( auto key_size=lsm->get(key_pre) ) {
    //           put_z.emplace_back(key_pre,std::to_string(n+std::stoi(key_size.value())));
    //   }else {
    //       put_z.emplace_back(key_pre,std::to_string(n));
    //   }
    //   lsm->put_batch(put_z);
    //   return ":"+std::to_string(n)+"\r\n";
}

std::string RedisWrapper::redis_zrem(std::vector<std::string> &args) {
  const std::string &key = args[1];
  std::unique_lock key_lock(key_mutex(key));
  if (expire_clean_locked(key)) {
    return ":0\r\n";
  }
  auto type = get_type_cached(key);
  if (type && *type != REDIS_TYPE_ZSET) {
    return REDIS_WRONGTYPE;
  }
  if (!type) {
    return ":0\r\n";
  }
  load_zset_cache_locked(key);

  std::unordered_map<std::string, std::string> removed_scores;
  int64_t current_size = 0;
  {
    std::shared_lock cache_lock(zset_cache_mtx_);
    const auto &elem_scores = zset_elem_score_cache_.at(key);
    current_size = zset_size_cache_.at(key);
    for (size_t i = 2; i < args.size(); ++i) {
      auto it = elem_scores.find(args[i]);
      if (it != elem_scores.end()) {
        removed_scores.emplace(it->first, it->second);
      }
    }
  }

  const int removed = static_cast<int>(removed_scores.size());
  if (removed == 0) {
    return ":0\r\n";
  }
  const int64_t new_size = current_size - removed;
  std::vector<std::string> remove_keys;
  for (const auto &[elem, score] : removed_scores) {
    remove_keys.emplace_back(get_zset_key_elem(key, elem));
    remove_keys.emplace_back(get_zset_key_socre(key, score, elem));
  }
  if (new_size == 0) {
    remove_keys.emplace_back(get_zset_key_preffix(key));
    remove_keys.emplace_back(get_explire_key(key));
    remove_keys.emplace_back(get_type_key(key));
  } else {
    lsm->put(get_zset_key_preffix(key), std::to_string(new_size));
  }
  lsm->remove_batch(remove_keys);
  if (new_size == 0) {
    erase_key_cache(key);
  } else {
    std::unique_lock cache_lock(zset_cache_mtx_);
    auto &elem_scores = zset_elem_score_cache_[key];
    for (const auto &[elem, score] : removed_scores) {
      (void)score;
      elem_scores.erase(elem);
    }
    zset_size_cache_[key] = new_size;
  }
  return ":" + std::to_string(removed) + "\r\n";
  // int count = 0;
  // std::vector<std::string> add_ve;
  // //std::unordered_set<std::string> s_del;
  // auto key_pre = std::move(get_zset_key_preffix(args[1]));
  // //set memtable,不存在或为0直接返回
  // auto key_size=std::move(lsm->get(key_pre));
  // //有点重复
  //  if (!key_size||std::stoi(key_size.value())==0) {
  //         return ":0\r\n";
  //  }
  // // 一次性把所有都拿出
  // // 然后空的为新,无则插入
  // for (int i = 2; i < a_s; ++i) {
  //   add_ve.emplace_back( get_zset_key_elem(args[1], args[i]));
  // }
  // //装载要删除的key

  // auto set_quary = std::move(lsm->get_batch(add_ve));
  //   add_ve={};
  //  for (auto& it : set_quary) {
  //          //有则删除
  //           if (it.second) {
  //              ++count;
  //             //elem
  //             add_ve.emplace_back(it.first);
  //             //score
  //             const auto& elem = it.first.substr(get_zset_elem_preffix(args[1]).size());
  //             add_ve.emplace_back(get_zset_key_socre(args[1],it.second.value(),elem));
  //           }
  //       }
  //   //删除score
  // //    auto s_pre=std::move(get_zset_score_preffix(args[1]));
  // //    auto tmp=std::move(lsm->lsm_iters_monotony_predicate(0, [&s_pre](const std::string& key){
  //    //在谓词查询中，是以key为准，因此大的时候，应该向左移动，即相反
  // //       return  -key.compare(0,s_pre.size(),s_pre);
  // // }));
  //     //删除set
  // //    auto begin=std::move(tmp->first);
  // //    auto end=std::move(tmp->second);
  // //    for (;begin!=end;++begin) {
  // //        if (s_del.contains((*begin).second)) {
  // //            add_ve.emplace_back((*begin).first);
  // //        }
  // //     }
     
  //    // count=std::stoi(key_size.value())-count;
  //     rlock.unlock();
  //    std::unique_lock<std::shared_mutex> wlock(redis_mtx);
  //    if (std::stoi(key_size.value())-count==0) {
  //        add_ve.emplace_back(key_pre);
  //        add_ve.emplace_back(get_explire_key(args[1]));
  //        add_ve.emplace_back(get_type_key(args[1]));
  //    }else {
  //       lsm->put(key_pre,std::to_string(std::stoi(key_size.value())-count));
  //    }
  //    lsm->remove_batch(add_ve);
  //    return  ":"+std::to_string(count)+"\r\n";

}
//0 -1 或者 ZRANGE key min max BYSCORE默认闭区间
//rev逆序，开区间的附加不实现
std::string RedisWrapper::redis_zrange(std::vector<std::string> &args) {
    std::shared_lock<std::shared_mutex> key_lock(key_mutex(args[1]));
     //是否有旧的key存在且过期
    if (expire_clean(args[1], key_lock)) {
            return "*0\r\n";
    }
    auto type = get_type_cached(args[1]);
if (type && *type != REDIS_TYPE_ZSET) {
  return REDIS_WRONGTYPE;
}
  if (!type) {
    return "*0\r\n";
  }
 
  auto score_pre=get_zset_score_preffix(args[1]);
  auto score_quary=std::move(lsm->lsm_iters_monotony_predicate(0,[&score_pre](const std::string& key){
        return -key.compare(0,score_pre.size(),score_pre);
  }));
  if (!score_quary) {
    return "*0\r\n";
  }
  //负数表示从尾部倒数
  int first=std::stoi(args[2]);
  int last=std::stoi(args[3]);
  int count=0;
  std::vector<std::pair<std::string,std::string>> sort;
  std::string ans;
  auto begin=std::move(score_quary->first);
  auto end=std::move(score_quary->second);
  //小写应该兼容,socre的可以用这个方式，seq的由于可以有倒数，因此要先解析全部
  if (args[args.size()-1]=="BYSCORE"||args[args.size()-1]=="BYSCORE") {
      for (;begin!=end;++begin) {
      if (last<std::stoi(get_zset_score_item((*begin).first))) {
            break;
          }
         if (std::stoi(get_zset_score_item((*begin).first))>=first) {
            ++count;
            //默认升序
            ans+="$"+std::to_string((*begin).second.size())+"\r\n"+(*begin).second+"\r\n";
         }
      }
  }else {
          for (;begin!=end;++begin) {
            sort.emplace_back((*begin).first,(*begin).second);
          }
          if (sort.empty()) {
            return "*0\r\n";
          }
    //倒数+size+1,但是是数组只用加size即可
    if (first<0) {
      first+=sort.size();
    }
   if (last<0) {
     last+=sort.size();
   }
   //倒数的个数超过了总数，即从头开始
   if (first<0) {
      first=0;
   }
   //结束位置不能小于第一个
   if (first>last||first>=(int)sort.size()) {
      return "*0\r\n";
   }
   last=std::min((int)sort.size() - 1,last);
    count=last-first+1;
   //防止last越界
   last=std::min((int)sort.size() - 1,last);
   for (;first<=last;++first) {
     ans+="$"+std::to_string(sort[first].second.size())+"\r\n"+sort[first].second+"\r\n";
   }
   //count=last-first+1;
  }
  //不在区间内
  if (ans=="") {
     return "*0\r\n";
  }
  return "*"+std::to_string(count)+"\r\n"+ans;
};

std::string RedisWrapper::redis_zcard(const std::string &key) {
     std::shared_lock<std::shared_mutex> key_lock(key_mutex(key));
      //是否有旧的key存在且过期
      if (expire_clean(key,key_lock)) {
       return ":0\r\n";
      }
     auto type = get_type_cached(key);
     if (type && *type != REDIS_TYPE_ZSET) {
       return REDIS_WRONGTYPE;
     }
  if (!type) {
    return ":0\r\n";
  }

  load_zset_cache_locked(key);
  std::shared_lock cache_lock(zset_cache_mtx_);
  auto size_it = zset_size_cache_.find(key);
  if (size_it != zset_size_cache_.end()) {
    return ":" + std::to_string(size_it->second) + "\r\n";
  }
  // //无特殊情况，不会出现非过期但是无数量
  //  if (auto set_quary=lsm->get(get_zset_key_preffix(key))) {
  //        return ":" + set_quary.value() + "\r\n";
  //  }
    return ":0\r\n";

}

std::string RedisWrapper::redis_zscore(const std::string &key,
                                       const std::string &elem) {
 std::shared_lock<std::shared_mutex> key_lock(key_mutex(key));
 //是否有旧的key存在且过期
 if (expire_clean(key, key_lock)) {
   return "$-1\r\n";
 }
 auto type = get_type_cached(key);
 if (type && *type != REDIS_TYPE_ZSET) {
   return REDIS_WRONGTYPE;
 }
 //代表没有key
 if (!type) {
   return "$-1\r\n";
 }
  load_zset_cache_locked(key);
  // //无特殊情况，不会出现非过期但是无数量
  //  if (auto set_quary=lsm->get(get_zset_key_elem(key,elem))) {
  //        return "$" + std::to_string(set_quary.value().size()) + "\r\n" + set_quary.value() + "\r\n";
  //  }
  std::shared_lock cache_lock(zset_cache_mtx_);
  auto cache_it = zset_elem_score_cache_.find(key);
  if (cache_it != zset_elem_score_cache_.end()) {
    auto score_it = cache_it->second.find(elem);
    if (score_it != cache_it->second.end()) {
      return "$" + std::to_string(score_it->second.size()) + "\r\n" +
             score_it->second + "\r\n";
    }
  }
    return "$-1\r\n";

}

std::string RedisWrapper::redis_zincrby(const std::string &key,
                                        const std::string &increment,
                                        const std::string &elem) {
      std::unique_lock key_lock(key_mutex(key));
      expire_clean_locked(key);
      if (!check_or_create_type_locked(key, REDIS_TYPE_ZSET)) {
        return REDIS_WRONGTYPE;
      }
     load_zset_cache_locked(key);

     std::vector<std::pair<std::string, std::string>> put_z;
     std::string new_score;
     std::optional<std::string> old_score;
     int64_t current_size = 0;
     {
       std::shared_lock cache_lock(zset_cache_mtx_);
       const auto &elem_scores = zset_elem_score_cache_.at(key);
       auto it = elem_scores.find(elem);
       if (it != elem_scores.end()) {
         old_score = it->second;
       }
       current_size = zset_size_cache_.at(key);
     }

     const bool added = !old_score.has_value();
     const int64_t new_size = current_size + (added ? 1 : 0);
     if (added) {
       new_score = increment;
       put_z.emplace_back(get_zset_key_preffix(key), std::to_string(new_size));
     } else {
       new_score =
           std::to_string(std::stoi(*old_score) + std::stoi(increment));
       put_z.emplace_back(get_zset_key_socre(key, *old_score, elem), "");
     }

     put_z.emplace_back(get_zset_key_elem(key, elem), new_score);
     put_z.emplace_back(get_zset_key_socre(key, new_score, elem), elem);

     lsm->put_batch(put_z);
     {
       std::unique_lock cache_lock(zset_cache_mtx_);
       zset_elem_score_cache_[key][elem] = new_score;
       zset_size_cache_[key] = new_size;
     }
     return ":" + new_score + "\r\n";
     //   //过期清理后，或者没有key
     //   if (const auto&
     //   key_quary=lsm->get(get_zset_key_preffix(key));!key_quary) {

     //       //如果key不存要创建
     //       std::vector<std::pair<std::string, std::string>> add;
     //       //value
     //       add.emplace_back(get_zset_key_elem(key,elem),increment);
     //     add.emplace_back(get_zset_key_socre(key,increment,elem),elem);
     //     //typr
     //     add.emplace_back(get_type_key(key),REDIS_TYPE_ZSET);
     //     //szie
     //     add.emplace_back(get_zset_key_preffix(key),"1");
     //     lsm->put_batch(add);
     //     return ":"+increment+"\r\n";
     //   }
     //  // std::unique_lock wlock(redis_mtx);
     //   std::string new_score;
     //   std::vector<std::pair<std::string, std::string>> add;

     //   auto set_quary=lsm->get(get_zset_key_elem(key,elem));
     //   if(auto set_quary=lsm->get(get_zset_key_elem(key,elem))){
     //       new_score=std::to_string(std::stoi(set_quary.value())+std::stoi(increment));
     //       //删除旧score
     //       add.emplace_back(get_zset_key_socre(key,set_quary.value(),elem),"");
     //   }else {
     //     //新建即直接插入并返回
     //     new_score=increment;
     //     auto z_size=std::stoi(lsm->get(get_zset_key_preffix(key)).value());
     //     add.emplace_back(get_zset_key_preffix(key),std::to_string(z_size+1));
     //   }
     //   add.emplace_back(get_zset_key_elem(key,elem),new_score);
     //   add.emplace_back(get_zset_key_socre(key,new_score,elem),elem);
     //   lsm->put_batch(add);
     //   return ":"+new_score+"\r\n";
}

std::string RedisWrapper::redis_zrank(const std::string &key,
                                      const std::string &elem) {
    std::shared_lock<std::shared_mutex> key_lock(key_mutex(key));
     //是否有旧的key存在且过期
      if (expire_clean(key,key_lock)) {
        return "$-1\r\n";
      }
     // std::unique_lock wlock(redis_mtx);
    auto type = get_type_cached(key);
    if (type && *type != REDIS_TYPE_ZSET) {
      return REDIS_WRONGTYPE;
    }
    if (!type) {
      return "$-1\r\n";
    }
 
  auto score_pre=get_zset_score_preffix(key);
  auto set_quary=std::move(lsm->lsm_iters_monotony_predicate(0,[&score_pre](const std::string key){
        return  -key.compare(0,score_pre.size(),score_pre);
  }));
  if (!set_quary) {
    return "$-1\r\n";
  }
  int seq=0;
  bool is_exit=false;
   auto begin=std::move(set_quary->first);
     auto end=std::move(set_quary->second);
     for (;begin!=end;++begin) {
      //Redis 的 ZRANK 是从 0 开始，第一个应该返回 :0
         if ((*begin).second==elem) {
           is_exit=true;
            break;
         }
         ++seq;
      }
  if (is_exit) {
     return ":"+std::to_string(seq)+"\r\n";
  }
  return "$-1\r\n"; // 表示成员不存在
}

std::string RedisWrapper::redis_sadd(std::vector<std::string> &args) {
  const std::string &key = args[1];
  std::unique_lock key_lock(key_mutex(key));
  expire_clean_locked(key);
  if (!check_or_create_type_locked(key, REDIS_TYPE_SET)) {
    return REDIS_WRONGTYPE;
  }
  load_set_cache_locked(key);

  std::unordered_set<std::string> new_members;
  int64_t current_size = 0;
  {
    std::shared_lock cache_lock(set_cache_mtx_);
    const auto &members = set_member_cache_.at(key);
    current_size = set_size_cache_.at(key);
    for (size_t i = 2; i < args.size(); ++i) {
      if (!members.contains(args[i])) {
        new_members.emplace(args[i]);
      }
    }
  }

  const int count = static_cast<int>(new_members.size());
  const int64_t new_size = current_size + count;
  if (count > 0) {
    std::vector<std::pair<std::string, std::string>> puts;
    puts.reserve(new_members.size() + 1);
    const auto member_prefix = get_set_member_prefix(key);
    for (const auto &member : new_members) {
      puts.emplace_back(member_prefix + member, get_set_member_value());
    }
    puts.emplace_back(get_set_key_preffix(key), std::to_string(new_size));
    lsm->put_batch(puts);

    std::unique_lock cache_lock(set_cache_mtx_);
    auto &members = set_member_cache_[key];
    members.insert(new_members.begin(), new_members.end());
    set_size_cache_[key] = new_size;
  }
  return ":" + std::to_string(count) + "\r\n";
}

std::string RedisWrapper::redis_srem(std::vector<std::string> &args) {
  const std::string &key = args[1];
  std::unique_lock key_lock(key_mutex(key));
  if (expire_clean_locked(key)) {
    return ":0\r\n";
  }
  auto type = get_type_cached(key);
  if (type && *type != REDIS_TYPE_SET) {
    return REDIS_WRONGTYPE;
  }
  if (!type) {
    return ":0\r\n";
  }
  load_set_cache_locked(key);

  std::unordered_set<std::string> removed_members;
  int64_t current_size = 0;
  {
    std::shared_lock cache_lock(set_cache_mtx_);
    const auto &members = set_member_cache_.at(key);
    current_size = set_size_cache_.at(key);
    for (size_t i = 2; i < args.size(); ++i) {
      if (members.contains(args[i])) {
        removed_members.emplace(args[i]);
      }
    }
  }

  const int count = static_cast<int>(removed_members.size());
  if (count == 0) {
    return ":0\r\n";
  }
  const int64_t new_size = current_size - count;
  std::vector<std::string> remove_keys;
  remove_keys.reserve(removed_members.size() + 3);
  for (const auto &member : removed_members) {
    remove_keys.emplace_back(get_set_member_key(key, member));
  }
  if (new_size == 0) {
    remove_keys.emplace_back(get_set_key_preffix(key));
    remove_keys.emplace_back(get_type_key(key));
    remove_keys.emplace_back(get_explire_key(key));
  } else {
    lsm->put(get_set_key_preffix(key), std::to_string(new_size));
  }
  lsm->remove_batch(remove_keys);

  if (new_size == 0) {
    erase_key_cache(key);
  } else {
    std::unique_lock cache_lock(set_cache_mtx_);
    auto &members = set_member_cache_[key];
    for (const auto &member : removed_members) {
      members.erase(member);
    }
    set_size_cache_[key] = new_size;
  }
  return ":" + std::to_string(count) + "\r\n";
}

std::string RedisWrapper::redis_sismember(const std::string &key,
                                          const std::string &member) {
     std::shared_lock<std::shared_mutex> key_lock(key_mutex(key));
      //是否有旧的key存在且过期
      if (expire_clean(key,key_lock)) {
       return ":0\r\n";
      }
     auto type = get_type_cached(key);
     if (type && *type != REDIS_TYPE_SET) {
       return REDIS_WRONGTYPE;
     }
  // 过期
   //auto key_pre = std::move(get_set_member_prefix(key));
 
   if (auto set_quary=lsm->get(get_set_member_key(key,member))) {
         return ":" + set_quary.value() + "\r\n";
   }
   return ":0\r\n";
}
//集合的元素个数
std::string RedisWrapper::redis_scard(const std::string &key) {
      std::shared_lock<std::shared_mutex> key_lock(key_mutex(key));
       //是否有旧的key存在且过期
      if (expire_clean(key,key_lock)) {
       return ":0\r\n";
      }
     auto type = get_type_cached(key);
  if (type && *type != REDIS_TYPE_SET) {
    return REDIS_WRONGTYPE;
  }
  // 过期
  // auto key_pre = std::move(get_set_member_prefix(key));
  
  //无特殊情况，不会出现非过期但是无数量
   if (auto set_quary=lsm->get(get_set_key_preffix(key))) {
         return ":" + set_quary.value() + "\r\n";
   }
    return ":0\r\n";
}

std::string RedisWrapper::redis_smembers(const std::string &key) {
  std::shared_lock<std::shared_mutex> key_lock(key_mutex(key));
   //是否有旧的key存在且过期
      if (expire_clean(key,key_lock)) {
       return "*0\r\n";
      }
  // 过期
 //  auto key_pre = std::move(get_set_member_prefix(key));
 auto type = get_type_cached(key);
  if (type && *type != REDIS_TYPE_SET) {
    return REDIS_WRONGTYPE;
  }
 
  auto key_elem_pre=std::move(get_set_member_prefix(key));
  //查最新
  auto set_quary=std::move(lsm->lsm_iters_monotony_predicate(0, [&key_elem_pre](const std::string& key){
      //在谓词查询中，是以key为准，因此大的时候，应该向左移动，即相反
        return  -key.compare(0,key_elem_pre.size(),key_elem_pre);
  }));
  if (set_quary) {
      //用一个先把所有的都存储，计数，然后再拼接
      int count=0;
      std::string tmp;

      auto& begin=set_quary.value().first;
      auto end=set_quary.value().second;
      for (;begin!=end;++begin) {
        if ((*begin).first == get_set_key_preffix(key)) {
          continue;
        }
        //把member剥离
        auto member = (*begin).first.substr(key_elem_pre.size());
        tmp += "$" + std::to_string(member.size()) + "\r\n" + member + "\r\n";
        ++count;
      }
      return "*"+std::to_string(count)+"\r\n"+tmp;
  }
  //空的
   return "*0\r\n";
}
//新增
std::string RedisWrapper::hset_not_exitst(std::vector<std::string> &args) {
  const std::string &key = args[1];
  const std::string &field = args[2];
  std::unique_lock key_lock(key_mutex(key));
  expire_clean_locked(key);
  if (!check_or_create_type_locked(key, REDIS_TYPE_HASH)) {
    return ":0\r\n";
  }
  load_hash_cache_locked(key);

  int64_t current_size = 0;
  {
    std::shared_lock cache_lock(hash_cache_mtx_);
    const auto &fields = hash_field_cache_.at(key);
    if (fields.contains(field)) {
      return ":0\r\n";
    }
    current_size = hash_size_cache_.at(key);
  }

  const int64_t new_size = current_size + 1;
  lsm->put_batch({
      {get_hash_filed_key(key, field), args[3]},
      {get_hash_size_key(key), std::to_string(new_size)},
  });
  {
    std::unique_lock cache_lock(hash_cache_mtx_);
    hash_field_cache_[key].emplace(field);
    hash_size_cache_[key] = new_size;
  }
  return ":1\r\n";
}
} // namespace tiny_lsm
