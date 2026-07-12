# tiny-lsm 简历证据梳理

本文档基于 `tiny-lsm` 当前工作区源码、头文件、测试、`Readme.md`、`xmake.lua` 与最近 Git 记录整理。目标是给后续修改 `resume/resume-zh_CN.tex` 提供可核验素材；没有源码或测试支撑的内容不建议写入简历。

## 1. 整体架构与请求执行路径

### 1.1 直接 KV API 路径

`LSM::put/get/remove/get_batch/remove_batch` 是库式调用入口。普通写入路径为：

`LSM` 申请事务 ID -> WiscKey 阈值判断与 value 编码 -> 写 WAL -> 写入 `MemTable` 的当前 SkipList -> 事务结束标记写入 MemTable -> 达到阈值后冻结 MemTable -> 后台 flush 线程将 frozen MemTable 生成 L0 SST -> 后台 compact 线程将 L0/L1+ 合并。

读取路径为：

先查 `MemTable` 当前表与 frozen tables，再查 L0 SST，最后对 L1+ SST 按 key 范围二分定位；命中后通过 `resolve_value_try` 解析 WiscKey value 引用。

证据：

- `tiny-lsm/src/lsm/engine.cpp`
- `LSM::put`、`LSM::put_batch`、`LSM::remove`、`LSM::get`
- `LSMEngine::get`、`LSMEngine::get_batch`、`LSMEngine::put`、`LSMEngine::request_flush`、`LSMEngine::flush_worker`、`LSMEngine::compact_worker`
- `tiny-lsm/include/lsm/engine.h`

### 1.2 Redis/RESP 路径

`server` 目标使用 Asio 接收 RESP 数组请求，`RedisSession::handleRequest` 解析命令后通过 `handler.cpp` 分发到 `RedisWrapper`。`RedisWrapper` 将 Redis String/Hash/Set/List/ZSet 命令转换为底层 LSM KV 读写。

证据：

- `tiny-lsm/server/src/server.cpp`
- `RedisSession::do_read`、`RedisSession::handleRequest`
- `tiny-lsm/server/src/handler.cpp`
- `string2Ops`、`*_handler`
- `tiny-lsm/src/redis_wrapper/redis_wrapper.cpp`
- `RedisWrapper::redis_set`、`redis_hset_batch`、`redis_sadd`、`redis_zadd` 等

状态：已实现部分 RESP 命令解析与 RedisWrapper 后端转发；不是完整 Redis 协议或完整 Redis 命令集。

## 2. 相对原始 tiny-lsm 的新增或修改重点

Git 基线可见当前分支 `lab2` 在 `upstream/lab2` 之后有多次提交。`git diff upstream/lab2..HEAD --stat` 显示涉及 `src/lsm/engine.cpp`、`src/redis_wrapper/redis_wrapper.cpp`、`src/wal/*`、`src/memtable/*`、`src/sst/*`、`src/block/*`、`include/common/common.h`、测试等约 49 个文件。

近期提交证据：

- `153ca2e 完成对set，zet，hash的缓存，flush和compact的线程`
- `0e48c9c hash加入缓存，改到hash-batch`
- `98b260d 完善了wisckey和mvcc的compact`
- `c6f3748 wal的崩溃恢复之前都已经完成`
- `bc8c068 wisckey的resolve修改，commit添加vlog...`

可归纳的新增/修改：

- Redis Hash/Set/ZSet 的进程内懒加载缓存。
- Hash HSET 支持 field-value 批量处理路径。
- 后台 flush/compact 线程与条件变量触发机制。
- WiscKey value log 与 SST footer 兼容处理。
- WAL 分段、恢复、checkpoint 与清理逻辑。
- MVCC 版本可见性、compact 后版本保留、墓碑保留相关修正。
- BlockCache LRU-K 风格缓存实现。

## 3. Redis 数据结构到底层 KV 的映射

### 3.1 String

映射：

- 数据：原始 key -> value。
- 类型元信息：`REDIS_TYPE_` + key -> `string`。
- 过期时间：`REDIS_EXPIRE_` + key -> Unix 秒级时间戳。

主要函数：

- `RedisWrapper::redis_set`
- `RedisWrapper::redis_get`
- `RedisWrapper::redis_incr`
- `RedisWrapper::redis_decr`
- `RedisWrapper::redis_expire`
- `RedisWrapper::redis_ttl`

状态：已实现。

测试/benchmark：

- `tiny-lsm/test/test_redis.cpp`：`SetAndGet`、`IncrAndDecr`、`Expire`
- `Readme.md` benchmark：SET/GET/INCR QPS

### 3.2 Hash

映射：

- 类型元信息：`REDIS_TYPE_` + key -> `hash`。
- field 数据：`REDIS_HASH_FIELD_` + key + `_` + field -> value。
- size 元信息：`REDIS_HASH_SIZE_` + key -> field 数量。
- 旧版 field-list 方案仍有辅助函数和注释，但当前主要路径使用 field 前缀扫描与缓存。

主要函数：

- `get_hash_filed_key`
- `get_hash_field_prefix`
- `get_hash_size_key`
- `RedisWrapper::redis_hset_batch`
- `RedisWrapper::redis_hget`
- `RedisWrapper::redis_hdel`
- `RedisWrapper::redis_hkeys`
- `RedisWrapper::load_hash_cache_unlocked`

状态：已实现常用命令；完整 Redis Hash 语义未覆盖。

测试/benchmark：

- `tiny-lsm/test/test_redis.cpp`：`HSetAndHGet`、`HDel`、`HKeys`、`HExpire`
- `Readme.md` benchmark：HSET QPS

### 3.3 Set

映射：

- 类型元信息：`REDIS_TYPE_` + key -> `set`。
- 成员数据：`REDIS_SET_` + key + `_` + member -> `"1"`。
- size 元信息：`REDIS_SET_` + key + `_` -> 成员数量。

主要函数：

- `get_set_member_key`
- `get_set_member_prefix`
- `get_set_key_preffix`
- `RedisWrapper::redis_sadd`
- `RedisWrapper::redis_srem`
- `RedisWrapper::redis_sismember`
- `RedisWrapper::redis_scard`
- `RedisWrapper::redis_smembers`
- `RedisWrapper::load_set_cache_unlocked`

状态：已实现常用命令；完整 Redis Set 语义未覆盖。

测试/benchmark：

- `tiny-lsm/test/test_redis.cpp`：`SetOperations`
- `Readme.md` benchmark：SADD QPS

### 3.4 List

映射：

- 类型元信息：`REDIS_TYPE_` + key -> `list`。
- list 元信息：`REDIS_LIST_PREFIX_` + key + `_` -> `start_stop`。
- 元素数据：代码意图为 `REDIS_LIST_PREFIX_` + key + `_` + 32 位补零 seq -> value。

注意：当前 `redis_lpush/rpush/lpop/rpop/lrange` 中部分调用使用 `get_list_value_key(key, seq)` 而不是 `get_list_key_value`/`get_list_value_key` 的命名语义较混乱，实际字符串可能不是统一的 `REDIS_LIST_PREFIX_...` 形式；但 `test_redis.cpp` 覆盖了基本 List 行为。

主要函数：

- `get_list_key_seq`
- `get_list_key_prefix`
- `get_list_value_key`
- `RedisWrapper::redis_lpush`
- `RedisWrapper::redis_rpush`
- `RedisWrapper::redis_lpop`
- `RedisWrapper::redis_rpop`
- `RedisWrapper::redis_llen`
- `RedisWrapper::redis_lrange`

状态：部分实现。可写“实现 LPUSH/RPUSH/LPOP/RPOP/LLEN/LRANGE 的基础 KV 映射”，不建议写“高性能 List”或“完整 Redis List”。

测试/benchmark：

- `tiny-lsm/test/test_redis.cpp`：`ListOperations`
- `Readme.md` 明确说明 lpush/rpush QPS 很慢，设计需要优化

### 3.5 ZSet

映射：

- 类型元信息：`REDIS_TYPE_` + key -> `zset`。
- size 元信息：`REDIS_SORTED_SET_` + key + `_` -> 元素数量。
- elem -> score：`REDIS_SORTED_SET_` + key + `_ELEM_` + elem -> score。
- score -> elem：`REDIS_SORTED_SET_` + key + `_SCORE_` + fixed-width-score + `_` + elem -> elem。
- fixed-width score 长度来自 `REDIS_SORTED_SET_SCORE_LEN = 32`。

主要函数：

- `get_zset_key_elem`
- `get_zset_key_socre`
- `get_zset_key_preffix`
- `get_zset_score_preffix`
- `get_zset_elem_preffix`
- `RedisWrapper::redis_zadd`
- `RedisWrapper::redis_zrem`
- `RedisWrapper::redis_zrange`
- `RedisWrapper::redis_zcard`
- `RedisWrapper::redis_zscore`
- `RedisWrapper::redis_zincrby`
- `RedisWrapper::redis_zrank`
- `RedisWrapper::load_zset_cache_unlocked`

状态：已实现常用命令；不完整覆盖 Redis ZSet 选项。当前 `ZRANGE BYSCORE` 分支只做简化处理，开区间、REV 等注释中明确未实现。

测试/benchmark：

- `tiny-lsm/test/test_redis.cpp`：`ZSetOperations`
- `Readme.md` benchmark：ZADD QPS

## 4. Hash field、Set/ZSet member 缓存实现和作用

缓存均位于 `RedisWrapper` 私有成员中，是进程内缓存，不是持久化结构。

Hash 缓存：

- `hash_field_cache_`: key -> field 集合。
- `hash_size_cache_`: key -> field 数量。
- `hash_loaded_`: 标记某个 hash 是否已从 LSM 前缀扫描加载。
- `load_hash_cache_unlocked` 首次访问时通过 `lsm_iters_monotony_predicate` 扫描 `REDIS_HASH_FIELD_` + key + `_` 前缀并填充缓存。
- `redis_hset_batch` 使用缓存判断新增 field 数量并批量写 field KV 和 size。
- `redis_hdel` 和 `redis_hkeys` 使用缓存维护删除和枚举。

Set 缓存：

- `set_member_cache_`: key -> member 集合。
- `set_size_cache_`: key -> member 数量。
- `set_member_loaded_`: 标记是否已加载。
- `load_set_cache_unlocked` 扫描 `REDIS_SET_` + key + `_` 前缀，跳过 size key，填充成员集合。
- `redis_sadd` 使用缓存避免重复成员并批量写入新增 member。

ZSet 缓存：

- `zset_elem_score_cache_`: key -> elem -> score。
- `zset_size_cache_`: key -> 元素数量。
- `zset_loaded_`: 标记是否已加载。
- `load_zset_cache_unlocked` 扫描 `_ELEM_` 前缀，恢复 elem 到 score 的映射。
- `redis_zadd`、`redis_zrem`、`redis_zscore`、`redis_zcard`、`redis_zincrby` 使用该缓存减少重复 get_batch/前缀扫描，并维护 size 与旧 score tombstone。

作用边界：

- 能说明“为 Hash/Set/ZSet 增加进程内懒加载成员索引，减少新增/删除时的重复底层查询，并维护 size 元信息”。
- 不能说明“缓存提升了多少性能”，因为没有专门缓存命中率或 A/B benchmark。
- 不能说明“重启后缓存保留”，因为缓存仅在内存中，重启后通过前缀扫描重建。

状态：已实现，缺少专门缓存测试和性能对比。

## 5. WAL、MemTable、SST、Compaction、事务模块真实完成状态

### 5.1 WAL

实现内容：

- `Record` 编码/解码。
- `WAL::log` 将记录缓冲到 `log_buffer_`，达到阈值或强制 flush 时 append 到 WAL 文件并 sync。
- `WAL::recover` 扫描 `wal.*` 文件，仅保留 checkpoint 之后且包含 commit 记录的事务。
- `WAL::reset_file` 支持 WAL 文件滚动。
- `WAL::cleaner`/`cleanWALFile` 后台清理旧 WAL 文件。

主要文件/函数：

- `tiny-lsm/src/wal/wal.cpp`
- `WAL::log`、`WAL::flush`、`WAL::recover`、`WAL::cleanWALFile`、`WAL::reset_file`
- `tiny-lsm/src/wal/record.cpp`
- `Record::encode`、`Record::decode`

状态：已实现同步 WAL、恢复、分段和清理；异步 WAL 未实现，README 也标记 `Async Wal` 未完成。

测试支撑：

- `tiny-lsm/test/test_wal.cpp`
- `LogAndFlush`、`RecoverTest`、`PartialFlushRecoveryTest`、`ConcurrentTransactionsTest`、`HighConcurrencyWithAborts`、`MixedTransactionCommitAbortAndCrashRecovery`

### 5.2 MemTable

实现内容：

- 使用 SkipList 作为 active MemTable。
- 支持 frozen tables，用于达到阈值后的 flush。
- 支持 put/get/remove/get_batch、迭代器、前缀/谓词查询。
- flush 时按 watermark 过滤 MVCC 历史版本，并保留必要版本。

主要文件/函数：

- `tiny-lsm/include/memtable/memtable.h`
- `tiny-lsm/src/memtable/memtable.cpp`
- `MemTable::put`、`get`、`get_batch`、`remove`
- `MemTable::frozen_cur_table_`
- `MemTable::flush_last`
- `MemTable::iters_monotony_predicate`

状态：已实现。

测试支撑：

- `tiny-lsm/test/test_memtable.cpp`
- 覆盖基础操作、冻结表、复杂迭代、谓词查询等。

### 5.3 SST / Bloom Filter / BlockCache / WiscKey

实现内容：

- SST 文件包含 block、metadata、Bloom Filter、footer。
- `SST::find_block_idx` 使用 Bloom Filter 快速排除并二分定位 block。
- `SST::read_block` 使用 `BlockCache` 缓存 block。
- `SSTBuilder` 写入 block/meta/bloom/footer，并支持 WiscKey footer magic。
- WiscKey 阈值由配置控制，当前默认 `WISCKEY_VALUE_THRESHOLD = 12`。
- `LSMEngine::tran_vlog` / `tran_vlog_batch` 将大 value 写入 `VLog` 并把 `[offset:uint64][size:uint32]` 引用写入 LSM。
- `LSMEngine::resolve_value_try` 在读路径解析 value log 引用。

主要文件/函数：

- `tiny-lsm/src/sst/sst.cpp`
- `SST::open`、`SST::read_block`、`SST::find_block_idx`、`SSTBuilder::add`、`SSTBuilder::build`
- `tiny-lsm/src/vlog/vlog.cpp`
- `VLog::append`、`VLog::append_batch`、`VLog::read_value`
- `tiny-lsm/src/block/block_cache.cpp`
- `BlockCache::get`、`BlockCache::put`、`BlockCache::hit_rate`

状态：已实现基础 SST、Bloom Filter、BlockCache、WiscKey value 分离。BlockCache 有单测；WiscKey 有专项测试。

测试支撑：

- `tiny-lsm/test/test_sst.cpp`
- `tiny-lsm/test/test_utils.cpp` 的 BloomFilter 测试
- `tiny-lsm/test/test_block_cache.cpp`
- `tiny-lsm/test/test_wisckey.cpp`

### 5.4 Compaction

实现内容：

- 后台 compact 线程接收请求后从 L0 开始执行 `full_compact`。
- L0->L1 使用多个 L0 SST 的 merge iterator 与 L1 concat iterator 合并。
- L1+ 使用相邻 level 的 concat iterator 合并。
- `gen_sst_from_iter` 在生成新 SST 时按事务 watermark 保留活跃事务可见所需版本，并避免在同一 key 的不同版本之间切分。
- compact 后删除旧 SST 文件并更新 `ssts` 与 `level_sst_ids`。

主要文件/函数：

- `tiny-lsm/src/lsm/engine.cpp`
- `LSMEngine::compact_worker`
- `LSMEngine::full_compact`
- `LSMEngine::full_l0_l1_compact`
- `LSMEngine::full_common_compact`
- `LSMEngine::gen_sst_from_iter`
- `tiny-lsm/src/sst/concact_iterator.cpp`
- `tiny-lsm/src/lsm/two_merge_iterator.cpp`

状态：已实现基础 full compaction 与 MVCC/墓碑保留修正；未证明工业级并发 compact 正确性。

测试支撑：

- `tiny-lsm/test/test_compact.cpp`
- `Persistence`
- `MultiVersionSurvivedAfterL0Compact`
- `MultiVersionSurvivedAfterL1Compact`
- `TombstoneSurvivedAfterL0Compact`
- `TombstoneDefaultConfig`

### 5.5 事务与隔离级别

实现内容：

- `TranManager` 分配事务 ID，维护 active/ready/flushed 集合，写 WAL，恢复 checkpoint 之后的已提交事务。
- `TranContext` 提供 put/remove/get/commit/abort。
- READ UNCOMMITTED 直接写 MemTable，并在 abort 时用 rollback_map 回滚。
- READ COMMITTED/REPEATABLE READ 先写 temp_map，commit 时写 WAL 和 MemTable。
- REPEATABLE READ 使用 `read_map_` 缓存第一次读取结果。
- 写冲突通过 `LSMEngine::chech_write` 检查 MemTable/SST 中是否存在更高事务 ID 版本。
- `SERIALIZABLE` 仅有枚举，README 标记未完成，没有真实串行化实现。

主要文件/函数：

- `tiny-lsm/include/lsm/transaction.h`
- `tiny-lsm/src/lsm/transation.cpp`
- `TranContext::put`、`remove`、`get`、`commit`、`abort`
- `TranManager::new_tranc`、`write_to_wal`、`check_recover`、`add_flushed_tranc_ids`
- `tiny-lsm/src/lsm/engine.cpp`
- `LSMEngine::chech_write`

状态：部分实现。可写 MVCC 版本读、事务提交/回滚、WAL 恢复、RU/RC/RR 实验性隔离支持和写冲突检测；不能写 Serializable 或完整事务正确性。

测试支撑：

- `tiny-lsm/test/test_lsm.cpp`：`TranContextTest`
- `tiny-lsm/test/test_wal.cpp`：事务恢复、并发提交/abort 恢复测试
- `tiny-lsm/test/test_compact.cpp`：compact 后 MVCC 版本保留

## 6. benchmark 能证明什么，不能证明什么

来源：

- `tiny-lsm/Readme.md`
- 命令：`redis-benchmark -h 127.0.0.1 -p 6379 -c 100 -n 100000 -q -t SET,GET,INCR,SADD,HSET,ZADD`
- 环境：Win11 WSL Ubuntu 22.04、32GB 6000 RAM、Intel 12600K

可证明：

- 在上述单一环境和参数下，RESP server + RedisWrapper 路径对 SET/GET/INCR/SADD/HSET/ZADD 能跑通 `redis-benchmark`。
- README 给出的 QPS 分别为 SET 142653.36、GET 134589.50、INCR 132802.12、SADD 131233.59、HSET 123456.79、ZADD 126422.25 requests/s。
- p50 延迟在 README 数据中约 0.503ms 到 0.615ms。

不能证明：

- 不能证明底层 LSM KV API 的真实性能，因为 benchmark 测的是 Redis wrapper server，不是直接 KV API。
- 不能证明缓存带来的性能提升，因为没有缓存前后 A/B 对比。
- 不能证明高并发事务正确性或线性一致性。
- 不能证明完整 Redis 兼容性。
- 不能证明 List 性能；README 明确指出 lpush/rpush 很慢，设计需要优化。
- 不能证明不同机器、不同数据规模、长时间运行、重启后恢复场景的性能。

简历可用表达：

- “使用 redis-benchmark 在 Win11 WSL/Intel 12600K/100 并发/10 万请求条件下验证部分 RESP 命令链路，SET/GET/INCR/SADD/HSET/ZADD 均可完成压测；结果仅作为 RedisWrapper 链路的功能与吞吐参考。”

## 7. 可写入简历的候选结论与证据表

| 简历候选描述 | 对应源码文件 | 类名/函数名 | 状态 | 测试或 benchmark 支撑 |
|---|---|---|---|---|
| 基于 C++20 实现 LSM-tree KV 引擎，写入经 WAL 持久化后进入 SkipList MemTable，按阈值冻结并刷入 SST，读取按 MemTable、L0、新旧层级 SST 顺序查询。 | `src/lsm/engine.cpp`, `include/lsm/engine.h`, `src/memtable/memtable.cpp` | `LSM::put/get`, `LSMEngine::get`, `MemTable::put/get/flush_last` | 已实现 | `test_lsm.cpp`, `test_memtable.cpp`, `test_sst.cpp` |
| 为 LSM 引擎补充后台 flush/compact 线程，使用 mutex/condition_variable 触发冻结 MemTable 刷盘与 L0 起始的 full compaction。 | `src/lsm/engine.cpp`, `include/lsm/engine.h` | `start_flush_thread`, `flush_worker`, `request_flush`, `start_compact_thread`, `compact_worker`, `full_compact` | 已实现 | `test_compact.cpp` 间接覆盖；无专门并发线程正确性测试 |
| 实现 WAL 记录编码、缓冲刷盘、日志滚动、checkpoint 清理与重启恢复，仅重放 checkpoint 后已提交事务。 | `src/wal/wal.cpp`, `src/wal/record.cpp`, `src/lsm/transation.cpp` | `WAL::log`, `WAL::flush`, `WAL::recover`, `WAL::cleanWALFile`, `TranManager::check_recover` | 已实现同步 WAL；异步 WAL 未实现 | `test_wal.cpp` |
| 实现事务上下文与 MVCC 可见性，支持事务提交/回滚、RR 读缓存、写冲突检测，并在 compact 后保留活跃事务所需版本。 | `src/lsm/transation.cpp`, `src/lsm/engine.cpp`, `include/common/common.h` | `TranContext::get/commit/abort`, `LSMEngine::chech_write`, `gen_sst_from_iter` | 部分实现；Serializable 未实现 | `test_lsm.cpp`, `test_wal.cpp`, `test_compact.cpp` |
| 修复/完善 compaction 中 MVCC 多版本和墓碑保留逻辑，避免压缩后历史版本或删除标记丢失。 | `src/lsm/engine.cpp`, `src/memtable/memtable.cpp`, `src/lsm/two_merge_iterator.cpp` | `full_l0_l1_compact`, `full_common_compact`, `gen_sst_from_iter`, `MemTable::flush_last` | 已实现 | `test_compact.cpp` 的 MVCC 与 tombstone 测试 |
| 实现 SST block metadata、Bloom Filter、BlockCache 和 key 范围定位，支持按 block 缓存读取。 | `src/sst/sst.cpp`, `src/block/block_cache.cpp`, `src/utils/bloom_filter.cpp` | `SST::find_block_idx`, `SST::read_block`, `BlockCache::get/put`, `BloomFilter::*` | 已实现 | `test_sst.cpp`, `test_block_cache.cpp`, `test_utils.cpp` |
| 增加 WiscKey 风格 value log，大 value 写入 VLog，SST/MemTable 中保存 offset+size 引用，读路径透明解析。 | `src/vlog/vlog.cpp`, `src/lsm/engine.cpp`, `src/sst/sst.cpp`, `include/vlog/vlog.h` | `VLog::append/append_batch/read_value`, `LSMEngine::tran_vlog`, `tran_vlog_batch`, `resolve_value_try`, `SST::open` | 已实现 | `test_wisckey.cpp` |
| 实现部分 RESP 命令解析与 RedisWrapper，支持 String、Hash、Set、List、ZSet 的常用命令并映射到底层 LSM KV。 | `server/src/server.cpp`, `server/src/handler.cpp`, `src/redis_wrapper/redis_wrapper.cpp` | `RedisSession::handleRequest`, `string2Ops`, `RedisWrapper::redis_*` | 已实现常用命令；非完整 Redis | `test_redis.cpp`, README benchmark |
| 为 Redis Hash/Set/ZSet 增加进程内懒加载成员缓存，首次访问通过前缀扫描恢复成员集合，后续新增/删除维护缓存和 size 元信息。 | `include/redis_wrapper/redis_wrapper.h`, `src/redis_wrapper/redis_wrapper.cpp` | `load_hash_cache_unlocked`, `load_set_cache_unlocked`, `load_zset_cache_unlocked`, `redis_hset_batch`, `redis_sadd`, `redis_zadd`, `redis_zrem` | 已实现；缺少缓存专项测试 | `test_redis.cpp` 间接覆盖功能；无缓存性能 A/B |
| 使用 Redis 前缀编码将 Hash field、Set member、ZSet elem/score 拆解为独立 KV，并用类型元信息避免跨类型误操作。 | `src/redis_wrapper/redis_wrapper.cpp`, `config.toml` | `get_type_key`, `check_or_create_type_unlocked`, `get_hash_filed_key`, `get_set_member_key`, `get_zset_key_elem`, `get_zset_key_socre` | 已实现 | `test_redis.cpp` |
| 使用 redis-benchmark 在指定环境验证部分 RESP 命令链路可跑通，并记录 SET/GET/INCR/SADD/HSET/ZADD 吞吐。 | `Readme.md` | benchmark 文档段落 | 已有数据；仅能作为参考 | README benchmark；未在本次重新运行 |

## 8. 不建议写入简历或必须降级表述的内容

- 不写“完整实现 Redis”或“兼容 Redis”。只能写“兼容部分 RESP 命令和 Redis 常用数据结构命令”。
- 不写“Serializable 隔离级别”。源码只有枚举，README 标为未完成。
- 不写“异步 WAL”。README 标为未完成，当前实现是同步落盘和后台清理。
- 不写“证明事务完全正确”或“线性一致”。测试覆盖有限，且事务实现是实验性质。
- 不写“缓存显著提升性能”。没有缓存前后 benchmark。
- 不写“List 高性能”。README 明确指出 lpush/rpush 慢且设计待优化。
- 不写未提供条件的性能数字。若引用 README benchmark，必须附条件：Win11 WSL Ubuntu 22.04、32GB 6000 RAM、Intel 12600K、100 并发、10 万请求、RedisWrapper server 链路。
