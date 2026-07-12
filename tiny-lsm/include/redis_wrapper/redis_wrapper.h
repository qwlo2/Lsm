#pragma once
#include "lsm/engine.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
namespace tiny_lsm {
std::vector<std::string>
get_fileds_from_hash_value(const std::optional<std::string> &field_list_opt);

std::string get_hash_value_from_fields(const std::vector<std::string> &fields);

inline std::string get_hash_filed_key(const std::string &key,
                                      const std::string &field);

inline bool is_value_hash(const std::string &key);

inline std::string get_explire_key(const std::string &key);



class RedisWrapper {
private:
  std::unique_ptr<LSM> lsm;
  std::shared_mutex redis_mtx;
  //set内存缓存
  std::unordered_map<std::string, std::string> type_cache_;
  std::unordered_map<std::string, int64_t> set_size_cache_;
  std::unordered_map<std::string, std::unordered_set<std::string>>
      set_member_cache_;
  //表示是否loaded
  std::unordered_set<std::string> set_member_loaded_;
//hash缓存
std::unordered_map<std::string, std::unordered_set<std::string>> hash_field_cache_;
std::unordered_map<std::string, int64_t> hash_size_cache_;
std::unordered_set<std::string> hash_loaded_;
//zset
std::unordered_map<std::string, std::unordered_map<std::string,std::string>> zset_elem_score_cache_;
std::unordered_map<std::string, int64_t> zset_size_cache_;
std::unordered_set<std::string> zset_loaded_ ;
private:
  // 检查 hash 的 key 是否过期并清理, 过期返回 true
  //这里都没有判断是否为统一类型，因此里面都套delete_key_unlocked
  //是未了防止第一次碰到已过期的旧类型
  //先expire请理过期旧类型，再判断统一命名空间，再add或get等
  bool expire_clean(const std::string &key,
                         std::shared_lock<std::shared_mutex> &rlock);
  bool expire_hash_clean(const std::string &key,
                         std::shared_lock<std::shared_mutex> &rlock);

  bool expire_list_clean(const std::string &key,
                         std::shared_lock<std::shared_mutex> &rlock);
  bool expire_zset_clean(const std::string &key,
                         std::shared_lock<std::shared_mutex> &rlock);
  bool expire_set_clean(const std::string &key,
                        std::shared_lock<std::shared_mutex> &rlock);
//检查统一空间
bool check_or_create_type_unlocked(const std::string &key,
                                                 const std::string &type) ;
//判断key是什么类型并删除
bool delete_key_unlocked(const std::string &key) ;
//清理单个key缓存
void erase_key_cache_unlocked(const std::string& key);
//第一次访问某个 set 时把各种信息加入
void load_set_cache_unlocked(const std::string& key);
//hash
void load_hash_cache_unlocked(const std::string& key);
//zset
void load_zset_cache_unlocked(const std::string& key) ;
public:
  RedisWrapper(const std::string &db_path);
  void clear();
  void flushall();

  // ************************* Redis Command Parser *************************
  // 基础操作
  std::string set(std::vector<std::string> &args);
  std::string get(std::vector<std::string> &args);
  std::string incr(std::vector<std::string> &args);
  std::string decr(std::vector<std::string> &args);
  std::string expire(std::vector<std::string> &args);
  std::string del(std::vector<std::string> &args);
  std::string ttl(std::vector<std::string> &args);
  // 哈希操作
  std::string hset(std::vector<std::string> &args);
  std::string hget(std::vector<std::string> &args);
  std::string hdel(std::vector<std::string> &args);
  std::string hkeys(std::vector<std::string> &args);
  // 链表操作
  std::string lpush(std::vector<std::string> &args);
  std::string rpush(std::vector<std::string> &args);
  std::string lpop(std::vector<std::string> &args);
  std::string rpop(std::vector<std::string> &args);
  std::string llen(std::vector<std::string> &args);
  std::string lrange(std::vector<std::string> &args);
  // 有序集合操作
  std::string zadd(std::vector<std::string> &args);
  std::string zrem(std::vector<std::string> &args);
  std::string zrange(std::vector<std::string> &args);
  std::string zcard(std::vector<std::string> &args);
  std::string zscore(std::vector<std::string> &args);
  std::string zincrby(std::vector<std::string> &args);
  std::string zrank(std::vector<std::string> &args);
  // 无序集合操作
  std::string sadd(std::vector<std::string> &args);
  std::string srem(std::vector<std::string> &args);
  std::string sismember(std::vector<std::string> &args);
  std::string scard(std::vector<std::string> &args);
  std::string smembers(std::vector<std::string> &args);
  //新增，应对注册
  std::string hsetnx(std::vector<std::string> &args);
private:
  // ************************* Redis Command Handler *************************
  // 基础操作
  std::string redis_incr(const std::string &key);
  std::string redis_decr(const std::string &key);
  std::string redis_expire(const std::string &key, std::string seconds_count);
  std::string redis_set(std::string &key, std::string &value);
  std::string redis_get(std::string &key);
  std::string redis_del(std::vector<std::string> &args);
  std::string redis_ttl(std::string &key);

  // 哈希操作
  std::string redis_hset(const std::string &key, const std::string &field,
                         const std::string &value);
  std::string redis_hset_batch(
      const std::string &key,
     const std::vector<std::pair<std::string, std::string>> &field_value_pairs);
  std::string redis_hget(const std::string &key, const std::string &field);
  std::string redis_hdel(const std::string &key, const std::string &field);
  std::string redis_hkeys(const std::string &key);
  // 链表操作
  std::string redis_lpush(const std::string &key, const std::string &value);
  std::string redis_rpush(const std::string &key, const std::string &value);
  std::string redis_lpop(const std::string &key);
  std::string redis_rpop(const std::string &key);
  std::string redis_llen(const std::string &key);
  std::string redis_lrange(const std::string &key, int start, int stop);
  // 有序集合操作
  std::string redis_zadd(std::vector<std::string> &args);
  std::string redis_zrem(std::vector<std::string> &args);
  std::string redis_zrange(std::vector<std::string> &args);
  std::string redis_zcard(const std::string &key);
  std::string redis_zscore(const std::string &key, const std::string &elem);
  std::string redis_zincrby(const std::string &key,
                            const std::string &increment,
                            const std::string &elem);
  std::string redis_zrank(const std::string &key, const std::string &elem);
  // 无序集合操作
  std::string redis_sadd(std::vector<std::string> &args);
  std::string redis_srem(std::vector<std::string> &args);
  std::string redis_sismember(const std::string &key,
                              const std::string &member);
  std::string redis_scard(const std::string &key);
  std::string redis_smembers(const std::string &key);
  
  //新增，应对注册
  std::string hset_not_exitst(std::vector<std::string> &args);
};
} // namespace tiny_lsm
