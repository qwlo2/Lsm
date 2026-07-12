# TinyLSM 面试准备材料

## 0. 证据范围与口径

本材料基于当前工作区源码、测试、构建脚本、README 和用户提供的 benchmark 数据整理，不把计划中或无法确认的能力写成已完成。

主要扫描证据：

| 方向 | 文件 | 类/函数 | 调用关系 |
|---|---|---|---|
| RESP Server | `tiny-lsm/server/src/server.cpp`、`tiny-lsm/server/include/handler.h`、`tiny-lsm/server/src/handler.cpp` | `RedisSession::do_read`、`RedisSession::handleRequest`、`*_handler` | Asio 读请求 -> RESP 解析 -> handler -> `RedisWrapper` |
| Redis 映射与缓存 | `tiny-lsm/include/redis_wrapper/redis_wrapper.h`、`tiny-lsm/src/redis_wrapper/redis_wrapper.cpp` | `RedisWrapper`、`check_or_create_type_unlocked`、`load_hash_cache_unlocked`、`load_set_cache_unlocked`、`load_zset_cache_unlocked`、`redis_hset_batch`、`redis_sadd`、`redis_zadd` | Redis 命令 -> 类型检查/过期清理 -> 缓存懒加载/增量维护 -> `LSM::put_batch/get/remove_batch` |
| LSM 核心读写 | `tiny-lsm/include/lsm/engine.h`、`tiny-lsm/src/lsm/engine.cpp` | `LSM`、`LSMEngine`、`put`、`put_batch`、`get`、`get_batch`、`remove`、`lsm_iters_monotony_predicate` | 外部 API -> WAL -> MemTable -> Frozen -> Flush -> SST -> Compaction |
| MemTable | `tiny-lsm/include/memtable/memtable.h`、`tiny-lsm/src/memtable/memtable.cpp` | `MemTable::put_`、`get`、`get_batch`、`frozen_cur_table_`、`flush_last` | 写入当前 SkipList；满后冻结；后台 flush 从 frozen 构建 SST |
| SkipList | `tiny-lsm/include/skiplist/skiplist.h`、`tiny-lsm/src/skiplist/skipList.cpp` | `SkipList::put`、`get`、`Iterator` | MemTable 底层有序结构，按 key 和事务版本组织 |
| SST/Block | `tiny-lsm/include/sst/sst.h`、`tiny-lsm/src/sst/sst.cpp`、`tiny-lsm/include/block/block.h`、`tiny-lsm/src/block/block.cpp` | `SST::open`、`SST::get`、`SST::read_block`、`SSTBuilder::add/build`、`Block::encode/decode/get_idx_binary` | Flush/Compaction 生成 SST；读路径按元数据定位 Block，再在 Block 内查 key |
| Bloom 与 Block Cache | `tiny-lsm/include/utils/bloom_filter.h`、`tiny-lsm/src/utils/bloom_filter.cpp`、`tiny-lsm/include/block/block_cache.h`、`tiny-lsm/src/block/block_cache.cpp` | `BloomFilter::add/possibly_contains`、`BlockCache::get/put/update_access_count` | SST 点查先过 Bloom；读 block 时查缓存，未命中再读文件 |
| WAL/恢复 | `tiny-lsm/include/wal/wal.h`、`tiny-lsm/src/wal/wal.cpp`、`tiny-lsm/include/wal/record.h`、`tiny-lsm/src/wal/record.cpp` | `WAL::log/flush/recover/cleanWALFile/reset_file`、`Record::encode/decode` | 写入先写 WAL；重启时从 checkpoint 之后恢复已提交事务 |
| 事务/MVCC | `tiny-lsm/include/lsm/transaction.h`、`tiny-lsm/src/lsm/transation.cpp` | `TranManager`、`TranContext::put/get/remove/commit/abort`、`get_oldest_active_tranc_id` | 事务分配 id；RU 直接写 MemTable 并保留 rollback；RC/RR 暂存到 `temp_map_`；commit 时写 WAL 再入 MemTable |
| WiscKey/VLog | `tiny-lsm/include/vlog/vlog.h`、`tiny-lsm/src/vlog/vlog.cpp`、`tiny-lsm/src/lsm/engine.cpp` | `VLog::append/append_batch/read_value`、`LSMEngine::tran_vlog/tran_vlog_batch/resolve_value_try` | 大 value 先写 VLog，MemTable/SST 保存 12 字节引用，读路径在 Engine resolve |
| 测试 | `tiny-lsm/test/*.cpp` | `test_lsm`、`test_wal`、`test_compact`、`test_wisckey`、`test_redis` 等 | 覆盖 SkipList、MemTable、Block、SST、WAL、Recover、Compaction、WiscKey、RedisWrapper |
| 构建与配置 | `tiny-lsm/xmake.lua`、`tiny-lsm/config.toml`、`tiny-lsm/Readme.md` | xmake targets、`TomlConfig` | C++20；配置 MemTable/SST/BlockCache/Bloom/WiscKey；README 标注部分 RESP 和性能记录 |

当前应保守表达的边界：

- Redis 只兼容部分 RESP 命令和部分数据结构语义，不能说完整 Redis。
- 事务实现包含教学型 RU/RC/RR 路径，不能说工业级事务正确性；单一事务 id 仍有可见性表达局限。
- WiscKey/VLog 有 append、batch append 和读取，源码注释明确 VLog GC 未实现。
- RedisWrapper 的 Hash/Set/ZSet 缓存是进程内缓存，重启后通过前缀扫描懒加载重建；不是持久化缓存。
- 后台 flush/compact 线程存在，但 compact 请求目前集中调用 `full_compact(0)`，属于简化实现。
- README 中也明确该项目是 educational project，并说明只支持 subset RESP。

## 1. 项目介绍

### 30 秒版本

TinyLSM 是一个 C++20 实现的单机持久化 KV 存储系统，底层是 LSM-Tree：写入先进入 WAL，再写 SkipList MemTable，MemTable 满后转成 Frozen MemTable 并刷成 SSTable，后台 Compaction 合并多层 SST。上层实现了部分 Redis RESP 命令，把 String、Hash、Set、List、ZSet 编码成内部 KV，并为 Hash/Set/ZSet 做了进程内成员索引缓存。项目用 GoogleTest 覆盖 WAL、恢复、Compaction、WiscKey 和 RedisWrapper；在 VMware、100 并发、10 万请求下热点 GET 约 20.3 万 QPS，百万随机空间下 SET/HSET/SADD/ZADD 约 5.7/5.1/5.2/2.5 万 QPS。

### 1 分钟版本

这个项目解决的是“把一个教学型 LSM 存储内核扩展成可通过 Redis 协议访问的持久化 KV 服务”。核心写链路是 `RedisSession::handleRequest` 解析 RESP 后进入 `RedisWrapper`，再调用 `LSM::put` 或 `put_batch`；`LSM` 先通过 `TranManager::write_to_wal` 写 WAL，再把数据写入 `MemTable::put_`。当 MemTable 超过配置阈值时，`MemTable::frozen_cur_table_` 把当前表转成 Frozen Table，后台 flush 线程调用 `LSMEngine::flush_one_frozens` 和 `MemTable::flush_last` 构建 SST。读链路先查 MemTable/Frozen，再查 SST，SST 点查使用 Bloom Filter 过滤不存在的 key，读取 block 时走 `BlockCache`。

我重点能讲的是三块：第一是 LSM 的 WAL-first、Flush、Compaction、Tombstone 和 MVCC 版本保留；第二是 Redis 数据结构到底层 KV 的编码，尤其 Hash/Set/ZSet 的前缀键和成员索引缓存；第三是 WiscKey，把大 value 追加到 `vlog.data`，在 MemTable/SST 中保存引用，读时由 Engine resolve。当前实现适合展示存储引擎和系统设计能力，但不能宣称完整 Redis 或工业级事务。

### 3 分钟版本

背景上，TinyLSM 是一个从零实现的 LSM-Tree KV 引擎，并在上层提供 Redis 兼容入口。整体架构从上到下分为 RESP Server、RedisWrapper、LSM API、WAL/MemTable、SST/Block、Compaction 和 VLog。服务端在 `server/src/server.cpp` 中用 Asio 接收客户端连接，`RedisSession::do_read` 读取 RESP 请求，`handleRequest` 解析成参数数组，再通过 `handler.cpp` 分发到 `RedisWrapper`。

存储写入遵循 WAL-first。比如 SET 会进入 `RedisWrapper::redis_set`，调用 `LSM::put_batch` 写业务 key 和类型 key；`LSM::put_batch` 先构造 `Record` 并调用 `TranManager::write_to_wal`，最后写入 `MemTable::put_`。MemTable 底层是 SkipList，支持同 key 多版本。超过 `LSM_PER_MEM_SIZE_LIMIT` 后，当前表通过 `frozen_cur_table_` 变成 Frozen，后台 flush 线程调用 `flush_one_frozens` 构建 SST 并发布到 L0。SST 由 Block、BlockMeta、Bloom Filter 和 footer 组成，`SST::get` 会先做范围和 Bloom 判断，再定位 block，`SST::read_block` 优先走 BlockCache。

Compaction 负责控制读放大和空间放大。`LSMEngine::full_compact` 先复制要参与合并的 SST 指针，然后释放全局 SST 锁做归并；L0 和 L1 通过 `full_l0_l1_compact` 合并，其他层通过 `full_common_compact` 合并。`gen_sst_from_iter` 会结合 `TranManager::get_oldest_active_tranc_id` 作为 watermark，保留活跃事务可能看到的新版本，并对 watermark 之前的历史版本只保留每个 key 的最新一条，从而避免把还可能被事务读取的历史版本或 Tombstone 过早删除。

Redis 层不是直接把复杂对象序列化成一个大 value，而是映射成多条内部 KV。Hash field 用 `REDIS_HASH_FIELD_ + key + "_" + field`；Set member 用 `REDIS_SET_ + key + "_" + member`；ZSet 同时维护 elem->score 和 score->elem 两个方向的键。为了减少热点请求反复前缀扫描，`RedisWrapper` 为 Hash/Set/ZSet 增加了进程内缓存：第一次访问时通过 `lsm_iters_monotony_predicate` 前缀扫描懒加载，后续 HSET/SADD/ZADD/HDEL/ZREM/SREM 进行增量维护。

优化方面，项目有 Bloom Filter、Block Cache 和 WiscKey。WiscKey 的当前实现是在 `LSMEngine::tran_vlog` 或 `tran_vlog_batch` 中把大 value 写到 VLog，再把 12 字节引用写入 MemTable/SST，读路径通过 `resolve_value_try` 还原。测试上，`test_wal` 覆盖日志与恢复，`test_compact` 覆盖 MVCC 和 Tombstone 版本保留，`test_wisckey` 覆盖 VLog/SST/LSM 集成，`test_redis` 覆盖常用 RedisWrapper 命令。性能方面，用户记录的 VMware 环境、100 并发、10 万请求下，热点 GET 可到约 20.3 万 QPS；百万随机空间下 SET/HSET/SADD/ZADD 分别约 5.7/5.1/5.2/2.5 万 QPS。

## 2. STAR 版本

**Situation：** 原始 KV 引擎具备 LSM 基础读写能力，但要支持 Redis 风格访问、复杂数据结构、崩溃恢复和较大数据量下的读写性能，需要把存储内核、协议层和数据结构映射串起来。

**Task：** 完善 WAL-first 写入、恢复、Flush/Compaction、WiscKey 和 RedisWrapper，让 String/Hash/Set/List/ZSet 常用命令能够通过 RESP Server 访问，并用测试和 benchmark 验证正确性与性能边界。

**Action：**

- 在 `LSM::put/put_batch/remove/remove_batch` 中写 WAL Record 后再写 MemTable，并通过 Frozen MemTable 和后台 flush 把内存数据转成 SST。
- 在 `WAL::recover` 中按 `wal.seq` 排序读取 Record，过滤 checkpoint 之前和未提交事务；`LSM::LSM` 重启时把恢复出的记录重放到 MemTable。
- 在 `gen_sst_from_iter` 和 `MemTable::flush_last` 中按事务 watermark 保留必要历史版本和 Tombstone，避免 compact 过早删除可见版本。
- 在 `RedisWrapper` 中用类型 key 和前缀 key 映射复杂类型，为 Hash/Set/ZSet 增加懒加载成员缓存和增量维护。
- 在 `LSMEngine::tran_vlog_batch/resolve_value_try` 中把大 value 转为 VLog 引用，并在读路径还原。

**Result：** GoogleTest 覆盖 WAL、Recover、Compaction、WiscKey、RedisWrapper 等模块；用户 benchmark 显示 VMware、100 并发、10 万请求下热点 GET 约 20.3 万 QPS，百万随机空间下 SET/HSET/SADD/ZADD 约 5.7/5.1/5.2/2.5 万 QPS。

**Reflection：** 项目证明了从存储内核到协议层的完整链路能力，但仍是教学型实现：RESP 覆盖有限，事务隔离不是工业级，VLog GC 未实现，后台线程和锁粒度仍有进一步优化空间。

## 3. 一张纸复习版

### 架构图

```text
Redis client
  -> Asio RedisSession::do_read / handleRequest
  -> handler.cpp: *_handler
  -> RedisWrapper: 类型键、前缀键、成员缓存、TTL 清理
  -> LSM: WAL-first put/get/remove/batch
  -> TranManager + WAL: Record 编码、日志滚动、checkpoint、recover
  -> MemTable: SkipList current + Frozen tables
  -> Flush: Frozen -> SSTBuilder -> SST(L0)
  -> Read SST: Bloom -> BlockMeta -> BlockCache -> Block
  -> Compaction: iterator merge -> version/Tombstone filter -> new SST
  -> Optional WiscKey: VLog append/read + Engine resolve
```

### 5 个核心亮点

1. WAL-first 写入和重启恢复：`LSM::put_batch` -> `TranManager::write_to_wal` -> `WAL::log` -> `MemTable::put_`；重启由 `WAL::recover` 和 `LSM::LSM` 重放已提交事务。
2. LSM 读写闭环：SkipList MemTable、Frozen MemTable、SST、Bloom、BlockCache、Flush、Compaction 全链路具备。
3. Redis 数据结构 KV 编码：类型键保证统一命名空间，Hash/Set/ZSet 用前缀 key 映射 field/member/score。
4. 成员索引缓存：Hash/Set/ZSet 首次前缀扫描懒加载，后续写入删除增量维护 size、member、member-score 映射。
5. MVCC/WiscKey 教学扩展：事务版本可见性、Compaction 版本保留、大 value VLog 引用和读时 resolve。

### 10 个最高频问题

1. 为什么 LSM 适合写多读少或追加写？
2. WAL 为什么必须先写？
3. MemTable 满后发生什么？
4. L0 和 L1+ 的查询策略为什么不同？
5. Bloom Filter 为什么主要优化点查？
6. Compaction 为什么要保留 Tombstone？
7. RR 为什么需要读缓存？
8. ZSet 为什么要 elem->score 和 score->elem 双索引？
9. 热点 benchmark 为什么比随机 benchmark 高？
10. 当前实现与工业级 Redis/LevelDB 的差距在哪里？

### 关键性能数字

- 环境：VMware、100 并发、10 万请求。
- 热点负载：GET 约 20.3 万 QPS。
- 百万随机空间：SET 约 5.7 万 QPS，HSET 约 5.1 万 QPS，SADD 约 5.2 万 QPS，ZADD 约 2.5 万 QPS。

### 3 个项目不足

- RESP/Redis 命令覆盖有限，部分命令语义与 Redis 官方仍有差距。
- 事务与 MVCC 是教学型实现，单一事务 id 对 read id/commit id/start id 的表达有限。
- VLog GC 未实现，后台 flush/compact 和 RedisWrapper 锁粒度还有优化空间。

### 3 个未来改进点

- 拆分 start/read/commit timestamp，完善快照读和冲突检测。
- 实现 VLog GC，并让 Compaction 与 VLog 存活引用协同。
- 优化 RedisWrapper 的锁粒度、批量写、缓存失效和 RESP 解析完整性。

## 4. 完整调用链

### 4.1 SET 写入链路

```text
Redis client
  -> tiny-lsm/server/src/server.cpp: RedisSession::do_read
  -> RedisSession::handleRequest
  -> tiny-lsm/server/src/handler.cpp: set_handler
  -> tiny-lsm/src/redis_wrapper/redis_wrapper.cpp: RedisWrapper::set / redis_set
  -> tiny-lsm/src/lsm/engine.cpp: LSM::put_batch
  -> tiny-lsm/src/lsm/transation.cpp: TranManager::write_to_wal
  -> tiny-lsm/src/wal/wal.cpp: WAL::log
  -> tiny-lsm/src/memtable/memtable.cpp: MemTable::put_
  -> MemTable 满: frozen_cur_table_ -> LSMEngine::request_flush
  -> 后台线程: LSMEngine::flush_worker -> flush_one_frozens
  -> MemTable::flush_last -> SSTBuilder::add/build
  -> 发布到 L0: level_sst_ids[0].emplace_front
  -> 需要时 request_compact -> full_compact
```

关键点：

- `RedisWrapper::redis_set` 写业务 key 和 `REDIS_TYPE_` 类型 key，并清理进程内缓存。
- `LSM::put_batch` 构造 put Record 和 commit Record，先写 WAL，再写 MemTable。
- 当前自动提交 `put` 不做乐观写冲突检查，写冲突主要体现在显式事务 `TranContext::commit`。

### 4.2 GET 查询链路

```text
Redis client
  -> RedisSession::handleRequest
  -> get_handler
  -> RedisWrapper::redis_get
  -> LSM::get
  -> LSMEngine::get
  -> MemTable::get(current -> frozen)
  -> SST levels:
       L0: 逐个 SST 查找
       L1+: 依据 first_key/last_key 做 lower_bound 范围筛选
  -> SST::get
  -> SST::find_block_idx: key range + Bloom Filter + BlockMeta
  -> SST::read_block: BlockCache::get miss 后读文件
  -> Block::get_idx_binary / BlockIterator
  -> LSMEngine::resolve_value_try
```

关键点：

- L0 内 SST 可能 key range 重叠，所以按 id 顺序逐个查。
- L1+ 经过 Compaction 后 key range 更接近有序不重叠，因此可以用 `lower_bound` 缩小候选。
- WiscKey 开启时，SST/MemTable 里可能是 12 字节引用，Engine 负责 resolve。

### 4.3 HSET/SADD 链路

HSET：

```text
handleRequest -> hset_handler
  -> RedisWrapper::hset
  -> RedisWrapper::redis_hset_batch
  -> expire_clean
  -> check_or_create_type_unlocked(key, "hash")
  -> load_hash_cache_unlocked(key)
  -> 更新 hash_field_cache_ / hash_size_cache_
  -> LSM::put_batch(REDIS_HASH_FIELD_key_field, value; REDIS_HASH_SIZE_key, size)
```

SADD：

```text
handleRequest -> sadd_handler
  -> RedisWrapper::sadd
  -> RedisWrapper::redis_sadd
  -> expire_clean
  -> check_or_create_type_unlocked(key, "set")
  -> load_set_cache_unlocked(key)
  -> set_member_cache_[key].insert(member)
  -> LSM::put_batch(REDIS_SET_key_member, "1"; REDIS_SET_key_, size)
```

关键点：

- 这两条链路都通过 `redis_mtx` 保证 RedisWrapper 级别的命令互斥/读写保护。
- 首次访问一个 key 时通过 LSM 前缀扫描构建缓存；之后同进程内新增/删除走缓存增量维护。

### 4.4 ZADD 链路

```text
handleRequest -> zadd_handler
  -> RedisWrapper::zadd
  -> RedisWrapper::redis_zadd
  -> check_or_create_type_unlocked(key, "zset")
  -> load_zset_cache_unlocked(key)
  -> zset_elem_score_cache_[key] 维护 elem -> score
  -> 新成员: 写 REDIS_SORTED_SET_key_ELEM_elem = score
             写 REDIS_SORTED_SET_key_SCORE_score_elem = elem
  -> 分数变化: tombstone 删除旧 score key，再写新 score key
  -> LSM::put_batch
```

为什么需要双索引：

- `ZSCORE key elem` 需要快速从 member 找 score，所以要 `ELEM_` 键。
- `ZRANGE key 0 -1` 或按 score 遍历，需要按 score 字典序扫描，所以要 `SCORE_` 键。
- 同 score 通过把 elem 拼到 score key 后面区分，避免同分覆盖。

### 4.5 WAL 恢复链路

```text
LSM::LSM(path)
  -> TranManager::read_tranc_id_file
  -> TranManager::init_new_wal
  -> TranManager::check_recover
  -> WAL::recover(data_dir, checkpoint)
       遍历 wal.* -> 按 seq 升序排序 -> Record::decode
       过滤 txn_id <= checkpoint
       只保留出现 OP_COMMIT 的事务记录
  -> LSM::LSM 重放:
       OP_PUT -> memtable.put_
       OP_DELETE -> memtable.remove_
       每个事务追加 sentinel ("", "", txn)
  -> add_ready_to_flush_tranc_id
  -> init_new_wal 新开后续 wal 文件
```

边界：

- `write_tranc_id_file` 主要在析构写入，源码注释也提示崩溃时不一定能析构，所以 checkpoint/事务 id 持久化仍是简化实现。
- `WAL::log` 在 buffer 达阈值或 `force_flush` 时写文件并 sync；`TranManager::write_to_wal` 当前调用 `wal->log(records, false)`，不等价于每个事务都强制 fsync。

### 4.6 MemTable Flush 链路

```text
LSM::put/put_batch/remove
  -> MemTable::put_ / remove_
  -> current_table->get_size() >= LSM_PER_MEM_SIZE_LIMIT
  -> MemTable::frozen_cur_table_
  -> LSMEngine::request_flush
  -> flush_thread_: LSMEngine::flush_worker
  -> LSMEngine::flush_one_frozens
       分配 sst_id
       创建 SSTBuilder
       watermark = TranManager::get_oldest_active_tranc_id
       MemTable::flush_last 构建 SST
       ssts_mtx 下发布到 ssts 和 level_sst_ids[0]
       MemTable::remove_flushed_table_
       TranManager::add_flushed_tranc_ids
       L0 满则 request_compact
```

关键点：

- `flush_last` 取最老 Frozen Table 构建 SST。
- 当前实现先发布 SST，再从 Frozen 表中移除，避免读请求在发布前看不到数据。

### 4.7 Compaction 链路

```text
L0 文件数量达到阈值 / request_compact
  -> compact_thread_: compact_worker
  -> full_compact(0)
  -> 拷贝 src/dst level SST id 与 shared_ptr
  -> 释放 ssts_mtx 后构造归并迭代器
      L0: SstIterator::merge_sst_iterator
      L1+: ConcactIterator
      TwoMergeIterator 做二路归并
  -> gen_sst_from_iter
      按 key 归并
      保留 txn > watermark 的版本
      对 txn <= watermark 只保留最新一条
      Tombstone 作为空 value 参与保留
  -> ssts_mtx 下删除旧 SST，插入新 SST
  -> 如果下一层超过阈值，递归 compact
```

Compaction 的作用：

- 降低读放大：减少同一层或多层重复文件。
- 降低空间放大：合并历史版本和删除标记。
- 代价：后台 I/O 和 CPU 消耗，可能影响前台写入延迟。

## 5. 高频问题分级

### A 级：必须熟练回答

#### A1. 为什么使用 LSM-Tree，而不是 B+Tree？

**30 秒简答：** LSM 把随机写转成 MemTable 内存写和 SST 顺序写，适合写多、吞吐优先的 KV 场景；代价是读路径要查多层 SST，需要 Bloom、BlockCache 和 Compaction 控制读放大。

**详细回答：** 当前项目写入从 `LSM::put_batch` 到 `MemTable::put_` 基本是内存追加/插入，Flush 时由 `MemTable::flush_last` 顺序生成 SST。B+Tree 更适合原地页更新和范围查询，但频繁随机写容易造成页分裂和随机 I/O。LSM 的问题是同一个 key 可能在 MemTable、L0、L1+ 中存在多个版本，所以读路径需要 `LSMEngine::get` 分层查找，SST 点查需要 `BloomFilter::possibly_contains` 和 `BlockCache::get` 优化。

**追问：** 那 LSM 的缺点是什么？答：读放大、写放大、空间放大和 Compaction 对前台延迟的影响。

#### A2. WAL 为什么必须先于 MemTable？

**30 秒简答：** 因为 MemTable 是内存结构，进程崩溃会丢；先写 WAL，崩溃后才能用 `WAL::recover` 重放已提交记录。

**详细回答：** SET 链路中 `LSM::put_batch` 先构造 `Record::putRecord` 和 `Record::commitRecord`，调用 `TranManager::write_to_wal`，再写 `MemTable::put_`。恢复时 `LSM::LSM` 调 `TranManager::check_recover`，内部通过 `WAL::recover` 读取 checkpoint 后的 `wal.*`，只保留已提交事务，再重放到 MemTable。如果顺序反过来，写入 MemTable 后还没写 WAL 就宕机，这条写入重启后就无法恢复。

**追问：** 当前实现是否每次 commit 都 fsync？答：不能这么说，`WAL::log` 支持 buffer 阈值和 force_flush，`write_to_wal` 当前传 false。

#### A3. append、flush、fsync 有什么区别？

**30 秒简答：** append 是写入用户态/内核页缓存路径；flush 是把程序缓冲区里的记录写到文件对象；fsync/sync 是要求操作系统把文件内容刷到磁盘。

**详细回答：** `WAL::log` 先把 Record 放入 `log_buffer_`，当 buffer 达到阈值或 force flush 时对每条 Record `encode` 后 `log_file_.append`，随后调用 `log_file_.sync`。`WAL::flush` 也会把 `log_buffer_` 里的记录 append 并 sync。VLog 的 `append_batch` 写入文件后没有每条都 sync，`SSTBuilder::build` 在 WiscKey 模式下会 sync vlog。面试里要区分“写入库内 buffer”“写入文件”“强制落盘”。

**追问：** 为什么不是所有写都 fsync？答：每次 fsync 延迟很高，通常用批量、组提交或可配置 durability 在吞吐和可靠性之间取舍。

#### A4. MemTable 满后发生什么？

**30 秒简答：** 当前 MemTable 被冻结成 Frozen/Immutable，新的写进入新 MemTable，后台 flush 线程把 Frozen 表刷成 SST 并发布到 L0。

**详细回答：** 在 `LSM::put_batch` 和事务 commit 后，会检查 `current_table->get_size() >= TomlConfig::getLsmPerMemSizeLimit()`。超过后调用 `MemTable::frozen_cur_table_`，它把当前 SkipList 放进 `frozen_tables` 并创建新的 SkipList。随后 `LSMEngine::request_flush` 唤醒 flush 线程，`flush_one_frozens` 调 `MemTable::flush_last` 构建 SST，最后更新 `ssts` 和 `level_sst_ids[0]`。

**追问：** 为什么不能直接清掉 Frozen？答：发布 SST 前读请求仍可能从 Frozen 读取，当前实现发布后再 `remove_flushed_table_`。

#### A5. L0 与 L1+ 有什么不同？

**30 秒简答：** L0 来自 MemTable flush，文件范围可能重叠；L1+ 经过 Compaction 后更接近有序不重叠，所以 L0 要逐个查，L1+ 可以按 key range 缩小候选。

**详细回答：** `LSMEngine` 构造时加载 SST：L0 按 sst_id 降序排序，其他层升序。`LSMEngine::get` 对 L0 遍历 `level_sst_ids[0]`；对 L1+ 使用 `lower_bound` 和 `first_key/last_key` 判断候选。这反映了 L0 overlap 和高层有序化的区别。

**追问：** L0 为什么按新到旧？答：同 key 多版本时，新 SST 更可能包含最新版本。

#### A6. Bloom Filter 为什么主要优化点查？

**30 秒简答：** Bloom 能快速判断“肯定不存在”，避免无意义读 SST；但范围查询需要按顺序扫描，Bloom 对连续范围帮助有限。

**详细回答：** `SSTBuilder::add` 会把 key 加入 Bloom，`SST::find_block_idx` 先检查 key 范围和 Bloom，再查 block meta。点查只关心单个 key，Bloom 可以在内存里过滤掉大量不相关 SST。范围查询是前缀/区间扫描，需要遍历有序迭代器，不可能对每个潜在 key 都依赖 Bloom。

**追问：** Bloom 有什么误判？答：可能 false positive，不能 false negative；命中了仍要查 block。

#### A7. Block Cache 如何工作？

**30 秒简答：** 读 SST block 时先用 `(sst_id, block_id)` 查 BlockCache，命中直接返回，未命中读文件并放入缓存；缓存内部按访问次数分 less_k 和 greater_k 两组链表。

**详细回答：** `SST::read_block` 调 `block_cache->get(sst_id, block_idx)`，未命中时按 BlockMeta 的 offset 从文件读取并 `Block::decode`，最后 `block_cache->put`。`BlockCache::update_access_count` 会增加 `access_count`，达到 k 后移到 greater_k 链表。它是 LRU-K 思路的简化实现，不等价于完整工业 LRU-K。

**追问：** 为什么缓存 block 而不是缓存 key？答：磁盘读取单位是 block，缓存 block 可以复用相邻 key 和范围扫描。

#### A8. Compaction 解决什么问题，又带来什么问题？

**30 秒简答：** Compaction 合并多层 SST，减少读放大和空间放大；代价是后台 CPU/I/O 和写放大，可能影响前台延迟。

**详细回答：** `full_compact` 复制参与合并的 SST，释放锁后用迭代器归并，再由 `gen_sst_from_iter` 输出新 SST。它能把旧版本和覆盖数据压缩掉，也能把 L0 重叠文件合并到 L1。但每次合并都要读旧 SST、写新 SST，带来额外 I/O，且若与前台读写共享锁和磁盘，会影响 latency。

**追问：** 为什么 LSM 仍然接受写放大？答：用顺序写换随机写，通常对写吞吐更友好。

#### A9. Tombstone 何时可以安全删除？

**30 秒简答：** 当没有活跃事务还可能读到 Tombstone 之前的版本，并且下层旧值也不会重新暴露时，才能在 Compaction 中删除。

**详细回答：** 当前项目用空 value 表示删除。`gen_sst_from_iter` 用 `get_oldest_active_tranc_id` 得到 watermark：`txn > watermark` 的版本都保留；`txn <= watermark` 只保留每个 key 的最新一条。这样 Tombstone 在仍可能屏蔽旧值时不会过早消失。测试 `test_compact.cpp` 有 `TombstoneSurvivedAfterL0Compact`、`TombstoneDefaultConfig` 等场景。

**追问：** 如果过早删除 Tombstone 会怎样？答：下层旧 value 可能重新被读出来，删除语义失效。

#### A10. MVCC 如何判断版本可见性？

**30 秒简答：** 每条记录带事务 id，查询传入可见事务 id；SkipList/Block 选择不大于该 id 的最新版本。

**详细回答：** `SkipList::get(key, tranc_id)` 对 `tranc_id != 0` 会找可见版本；`Block::get_idx_binary` 定位 key 后再按事务 id 调整到可见版本；`LSMEngine::get(key, tranc_id)` 会把这个 id 贯穿 MemTable 和 SST 查询。RR 在 `TranContext::get` 中还会把第一次读到的结果放入 `read_map_`，保证同事务重复读一致。

**追问：** 单一 transaction id 的局限？答：很难区分 start_ts/read_ts/commit_ts，对并发提交和快照边界表达不够精确。

#### A11. RU、RC、RR 在项目中如何实现？

**30 秒简答：** RU 直接写 MemTable 并记录 rollback；RC/RR 先写事务临时表，commit 时写 WAL 并落 MemTable；RR 对读结果做缓存。

**详细回答：** `TranContext::put/remove` 中 RU 会加锁并调用 `engine_->memtable.put_` 或 `remove_`，同时把旧值放进 `rollback_map_`；RC/RR 则写入 `temp_map_`。`TranContext::commit` 对非 RU 检查写冲突，写 WAL，再把 `temp_map_` 写入 MemTable。`TranContext::get` 对 RR 使用 `read_map_` 缓存第一次读取的值，RC 不缓存，RU 用 txn id 0 读最新。

**追问：** 这是完整隔离级别吗？答：不是，是教学型路径，缺少更完整的 timestamp/lock/validation 体系。

#### A12. WiscKey 为什么降低写放大？

**30 秒简答：** 大 value 不随 SST Compaction 反复重写，只把小的 value pointer 参与 LSM 合并，从而降低大 value 场景的写放大。

**详细回答：** `LSMEngine::tran_vlog` 判断 value 长度达到阈值后调用 `VLog::append`，返回 `[offset:uint64][size:uint32]` 组成的 12 字节引用。`tran_vlog_batch` 批量 append，减少多次追加开销。读时 `resolve_value_try` 根据引用调用 `VLog::read_value`。Compaction 只处理引用，不重新追加 VLog。边界是源码 `vlog.h` 注释明确 GC 没做。

**追问：** VLog 应在何时写入？答：必须在 WAL/MemTable 写入前完成引用转换，否则 WAL 和 MemTable 中不能保存一致的 value 表示。

#### A13. Redis Hash、Set、ZSet 如何映射到底层 KV？

**30 秒简答：** 用类型 key 维护统一命名空间，用前缀 key 拆分复杂结构成员：Hash field、Set member、ZSet elem/score 都是独立 KV。

**详细回答：** `get_type_key(key)` 生成 `REDIS_TYPE_key`，`check_or_create_type_unlocked` 用它判断 key 是否已被其他类型占用。Hash field key 是 `REDIS_HASH_FIELD_key_field`；Set member key 是 `REDIS_SET_key_member`；ZSet 有 `REDIS_SORTED_SET_key_ELEM_elem` 和 `REDIS_SORTED_SET_key_SCORE_score_elem`。这样复用 LSM 点查和前缀扫描，而不用维护一个大对象。

**追问：** 为什么不用一个 value 存整个集合？答：大集合每次修改都要重写整个 value，不适合 LSM 增量写。

#### A14. 成员索引缓存缓存什么，不缓存什么？

**30 秒简答：** 缓存 Hash fields、Set members、ZSet elem->score 和 size；不缓存完整 LSM Block，也不是持久化缓存。

**详细回答：** `redis_wrapper.h` 中有 `hash_field_cache_`、`set_member_cache_`、`zset_elem_score_cache_` 和 size cache。`load_*_cache_unlocked` 首次通过 `lsm_iters_monotony_predicate` 前缀扫描构建缓存；之后 HSET/SADD/ZADD/HDEL/ZREM 等增量维护。进程重启后缓存为空，首次访问会重建。

**追问：** 缓存如何失效？答：`erase_key_cache_unlocked` 在 DEL、SET 覆盖、清空等场景清理对应 key 的缓存。

#### A15. 热点 benchmark 与随机 benchmark 为什么差距大？

**30 秒简答：** 热点负载重复访问少量 key/member，能走内存缓存和 BlockCache；随机负载 key 空间大，缓存命中低，还会触发更多 MemTable、SST 和 Compaction 成本。

**详细回答：** 用户给的热点测试不带 `-r`，SADD/HSET/ZADD 会反复命中相同 key 和重复 member/field，RedisWrapper 的成员缓存能减少底层查询，所以数字很高。百万随机空间带 `-r 1000000`，成员新增多、前缀扫描和 LSM 写入更真实，ZADD 还要维护 elem->score 和 score->elem 两个索引并处理旧 score tombstone，所以最慢。随机 GET 命中率不明确，不能作为随机读结论。

**追问：** 数据能证明什么？答：能证明当前实现的热点读、部分随机写吞吐有一定能力；不能证明完整 Redis 兼容、工业可靠性或所有负载下性能。

### B 级：源码深入问题

#### B1. `LSMEngine::get` 为什么先查 MemTable 再查 SST？

**30 秒简答：** MemTable/Frozen 中的数据更新、版本更新，优先级高于磁盘 SST。

**详细回答：** `LSMEngine::get` 先调用 `memtable.get(key, tranc_id)`，如果返回有效且 value 非空就直接返回；如果 value 为空表示 Tombstone。只有 miss 时才在 `level_sst_ids` 中查 SST。这样保证新写覆盖旧 SST。

**追问：** Frozen 是否也参与读？答：参与，`MemTable::get` 内部先 current，再遍历 frozen tables。

#### B2. `WAL::recover` 为什么要按 `wal.seq` 排序？

**30 秒简答：** WAL 发生滚动后，恢复必须按写入顺序重放。

**详细回答：** `WAL::recover` 遍历 `wal.*`，解析 seq 后放入 vector，`std::sort` 升序处理。每个文件 `Record::decode` 后按事务 id 分组，过滤 checkpoint 之前和未提交事务。

**追问：** 如果不排序会怎样？答：后写的旧版本可能被先重放，破坏最终状态。

#### B3. `init_new_wal` 为什么不删除所有旧 WAL？

**30 秒简答：** 重启后仍可能有未 flush 但已提交的 WAL 需要恢复，不能启动时直接清理。

**详细回答：** `TranManager::init_new_wal` 创建 WAL 对象时传入 checkpoint；`WAL` 构造函数扫描已有 `wal.*`，选择最大 seq 后新开一个文件。旧文件交给 `WAL::recover` 和 cleaner/checkpoint 判断是否可清理。

**追问：** 清理依据是什么？答：`cleanWALFile` 检查旧文件中事务 id 是否都不大于 checkpoint，并保留当前活跃 WAL。

#### B4. `MemTable::flush_last` 如何处理多版本？

**30 秒简答：** 它遍历待 flush 的 SkipList，按 watermark 保留活跃事务可能看到的版本，并收集事务完成标记。

**详细回答：** `flush_last` 取 `frozen_tables.back()`，遍历 `table->flush()` 的 entries。对 sentinel `("", "")` 收集 flushed txn id；对普通 key，根据 `watermark` 保留 `txn > watermark` 的版本，对 `txn <= watermark` 每个 key 只保留最新一条。这与 Compaction 的过滤思路一致。

**追问：** 为什么要保留 `txn > watermark`？答：这些版本可能对当前或未来活跃事务仍然可见。

#### B5. `gen_sst_from_iter` 如何避免切分同一个 key？

**30 秒简答：** 构建 SST 时按 key 归并，并在接近大小限制时避免把同一 key 的版本拆到不同 SST。

**详细回答：** `gen_sst_from_iter` 用 `cur_key` 和 per-key 状态判断版本保留；当 builder 大小到阈值附近时也要考虑当前 key 的连续版本，避免同 key 多版本跨文件后查找复杂化。

**追问：** 如果同 key 版本跨文件会怎样？答：读和 compact 要额外处理多个候选文件，容易漏版本或错误覆盖。

#### B6. `resolve_value_try` 为什么放在 Engine？

**30 秒简答：** 因为 MemTable 和 SST 中都可能存 VLog 引用，把 resolve 放 Engine 可以统一读路径。

**详细回答：** `LSMEngine::get/get_batch/sst_get_` 取到 raw value 后调用 `resolve_value_try`。如果 WiscKey 关闭或 raw 小于 12 字节，直接返回；否则按 8 字节 offset 和 4 字节 size 读 VLog。这样 Compaction 可直接复用引用，不需要重新 append VLog。

**追问：** 12 字节普通 value 会不会歧义？答：项目通过 threshold <= 12 的规则尽量避免普通大 value 与引用混淆，但这仍是需要谨慎说明的实现约束。

#### B7. RedisWrapper 为什么有 `redis_mtx`？

**30 秒简答：** 复杂 Redis 命令会读写多条内部 KV 和进程内缓存，需要在命令级别保护一致性。

**详细回答：** HSET/SADD/ZADD 都会先查类型、清理 TTL、加载缓存、更新 size、写多条 KV。如果多个线程同时操作同一个 key，缓存和底层 KV 可能不一致。`redis_mtx` 用 shared/unique lock 粗粒度保护，但这也会限制并发性能。

**追问：** 如何优化？答：按 key 分片锁、缓存粒度锁、批量写合并、读写路径减少全局锁持有时间。

#### B8. ZSet 分数如何保证排序？

**30 秒简答：** score 被格式化成固定宽度字符串拼进 key，利用 LSM key 字典序扫描实现按分数遍历。

**详细回答：** `get_zset_key_socre` 用 `std::setw(REDIS_SORTED_SET_SCORE_LEN)` 和 `setfill('0')` 格式化 score，再生成 `..._SCORE_score_elem`。`redis_zrange` 对 `get_zset_score_preffix(key)` 做前缀扫描得到按 key 排序的结果。当前实现主要适合非负整数字符串分数，不要夸成完整 Redis sorted set。

**追问：** 负数、小数怎么办？答：当前代码使用 `stoi` 和字符串补零，完整支持需要可排序编码设计。

#### B9. List 是怎么映射的？

**30 秒简答：** List 用一个元信息 key 保存 start/end sequence，再用定长序号 key 存每个元素。

**详细回答：** `get_list_key_prefix(key)` 是元信息前缀，value 保存 `start_stop`；`get_list_value_key(key, seq)` 把 seq 格式化成 32 位宽度，生成元素 key。LPUSH 减 start，RPUSH 增 stop，LRANGE 前缀扫描后处理正负下标。

**追问：** 这个实现有什么风险？答：序号边界、空 list 清理、负数范围与 Redis 完整语义都需要更多测试。

#### B10. Recover 为什么只重放 committed 事务？

**30 秒简答：** 未提交事务崩溃后不应生效，否则会破坏原子性。

**详细回答：** `WAL::recover` 用 `committed` map 记录出现 `OP_COMMIT` 的事务，最后遍历 `ans` 删除未提交事务。`LSM::LSM` 只重放 recover 返回的记录。

**追问：** rollback Record 怎么处理？答：当前 recover 主要以 commit Record 判断是否保留，abort/rollback 记录更多用于事务状态标记，仍是简化实现。

#### B11. 为什么后台 compact 不能一直拿着 `ssts_mtx`？

**30 秒简答：** Compact 归并和建 SST 很耗时，如果全程持锁，会阻塞前台读写访问 SST 元数据。

**详细回答：** `full_compact` 先在锁内复制 SST id 和 shared_ptr，然后释放锁做 iterator merge 和新 SST 构建，最后重新加锁发布新 SST。这是典型的缩短临界区做法。

**追问：** 还有什么竞态？答：发布阶段和读路径要保证旧 SST 在读完前不释放，shared_ptr 有助于生命周期管理。

#### B12. Redis Server 的 RESP 解析有什么限制？

**30 秒简答：** `RedisSession::do_read` 用 `async_read_until("\r\n")` 和简化循环读取数组元素，不是完整流式 RESP 解析器。

**详细回答：** 代码假设请求已经按行进入 `asio::streambuf`，如果 `buffer_.size()==0` 就返回 partial request error。完整 Redis 协议需要状态机式解析，处理 pipelining、粘包、半包、多 bulk string 等情况。

**追问：** benchmark 为什么还能跑？答：redis-benchmark 常见命令格式简单，短请求下这套简化解析能覆盖一部分场景。

### C 级：压力追问

#### C1. 如果 WAL 写成功、MemTable 写失败怎么办？

**30 秒简答：** 恢复时会重放 WAL，最终仍可恢复；但当前代码对 MemTable 写入失败的异常路径处理不够完整。

**详细回答：** WAL-first 的好处是 WAL 成功后哪怕进程崩溃，重启能恢复。当前 MemTable 写入多为内存操作，失败主要是异常或资源问题；工业实现需要更完整的错误传播和回滚策略。

**追问：** 如果 WAL 未 sync 就崩溃？答：可能丢失 buffer 中未落盘记录，取决于 `WAL::log` 是否触发 sync。

#### C2. 为什么不把 Redis Hash 整体存在一个 value？

**30 秒简答：** 大 Hash 每次改一个 field 都要重写整个 value，不适合 LSM 的增量写和前缀扫描。

**详细回答：** 当前用 field key 拆分，每次 HSET 只写对应 field 和 size。代价是 HKEYS/HDEL 等需要前缀扫描或成员缓存，缓存一致性也更复杂。

**追问：** 什么时候整体 value 更好？答：小对象、字段很少、经常整对象读取时。

#### C3. ZADD 为什么比 SADD 慢？

**30 秒简答：** ZADD 要维护 elem->score 和 score->elem 两套索引，score 变化还要删除旧 score key。

**详细回答：** `redis_zadd` 对每个 elem 查 `zset_elem_score_cache_`；新成员写两条 KV，旧成员分数变化写旧 score tombstone 和新 score key。SADD 通常只写 member key 和 size，逻辑更轻。

**追问：** 如何优化 ZADD？答：更高效的 score 编码、减少全局锁、批量去重、缓存分片、跳表/内存索引辅助。

#### C4. 如果缓存和 LSM 底层不一致怎么办？

**30 秒简答：** 当前缓存由同一进程内 RedisWrapper 写路径维护，DEL/SET 会清理；崩溃重启后缓存丢失，再通过前缀扫描重建。

**详细回答：** 缓存不是持久化状态，正确性最终依赖 LSM 中的 KV。风险在于复杂命令如果绕过 RedisWrapper 或有并发更新未正确加锁，缓存会不一致。当前用 `redis_mtx` 粗粒度保护。

**追问：** 工业实现怎么做？答：要么把缓存作为派生状态并可重建，要么通过更严格的事务/锁/版本机制保证一致。

#### C5. Compaction 期间读到旧 SST 怎么办？

**30 秒简答：** 读路径拿到 shared_ptr，Compact 构建新 SST 后再发布并移除旧 id，shared_ptr 可以保证旧对象生命周期。

**详细回答：** `full_compact` 复制 shared_ptr 后释放锁合并；发布阶段才更新 `ssts` 和 level id。正在读旧 SST 的线程如果持有 shared_ptr，不会因 map 删除立即悬空。

**追问：** 文件删除呢？答：需要确保没有读者使用后再删除物理文件；当前源码更偏教学实现，不能夸成完整安全回收。

#### C6. 为什么随机 GET 不作为主要读性能结论？

**30 秒简答：** `redis-benchmark -r` 的 GET key 是否存在、命中率是多少不明确，吞吐混合了 miss 快路径和 hit 路径。

**详细回答：** 如果随机 GET 大量 miss，Bloom 会快速过滤，吞吐可能虚高；如果大量 hit，则会触发 block 读取和缓存行为。没有命中率统计就不能用它代表真实随机读。

**追问：** 应该怎么测？答：预加载数据集，分别测 100% hit、100% miss、Zipf 分布，并记录 p50/p99。

#### C7. 当前事务最大问题是什么？

**30 秒简答：** 单一事务 id 同时承担开始、读取和提交语义，难以表达完整快照隔离和提交顺序。

**详细回答：** 代码里 `tranc_id` 贯穿 Record、MemTable、SST 可见性和 active txn watermark。工业 MVCC 通常区分 start_ts、read_ts、commit_ts，并维护提交表和 GC safe point。

**追问：** 你会怎么改？答：引入 transaction id、read id、commit id，读路径按 read id 判断可见，提交后分配 commit id。

#### C8. WiscKey 没有 GC 会带来什么问题？

**30 秒简答：** 被覆盖或删除的大 value 仍留在 VLog，空间会持续增长。

**详细回答：** 当前 Compaction 只合并 LSM 中的引用和 Tombstone，不会扫描 VLog 并回收无效 value。长期运行需要 VLog GC：找出仍被最新 SST/MemTable 引用的 offset，复制存活 value，更新引用。

**追问：** GC 难点是什么？答：与 LSM Compaction、活跃事务可见性和崩溃恢复的一致性。

#### C9. 如果 redis_mtx 粗锁影响性能，怎么改？

**30 秒简答：** 按 key 或 key hash 分片锁，读缓存可用更细粒度锁，底层 LSM 批量写尽量缩短锁持有时间。

**详细回答：** 当前 RedisWrapper 的很多写命令会拿全局 unique_lock，导致不同 key 的 HSET/SADD/ZADD 也串行。可以设计 shard mutex 数组，以 logical key 定位锁；类型缓存、成员缓存也按 key 分片；LSM 层写入可继续用自己的锁。

**追问：** 分片锁会不会死锁？答：多 key 命令必须按固定顺序加锁或做事务化批量协议。

#### C10. 项目与 LevelDB/Redis 的差距？

**30 秒简答：** 它展示了核心机制，但在协议兼容、事务模型、资源回收、错误处理、并发调度和长期稳定性上都不是工业实现。

**详细回答：** 与 LevelDB 相比，缺少成熟 Manifest、VersionSet、compaction picker、严格 crash consistency、table cache 等；与 Redis 相比，RESP、命令语义、数据结构编码、事件循环和持久化模型都只覆盖子集。

**追问：** 面试里怎么说才稳？答：说“教学型/实验型 LSM KV，引入部分 Redis 兼容层”，不要说“实现了 Redis”。

## 6. 性能结果解读

用户提供并要求使用的数据：

热点负载：

```bash
redis-benchmark -h 127.0.0.1 -p 6380 -c 100 -n 100000 -q -t SET,GET,INCR,SADD,HSET,ZADD
```

- GET：约 20.3 万 QPS，p50 约 0.455 ms。

百万随机空间：

```bash
redis-benchmark -h 127.0.0.1 -p 6380 -c 100 -n 100000 -r 1000000 -q -t SET,GET,INCR,SADD,HSET,ZADD
```

- SET：约 5.7 万 QPS，p50 约 1.623 ms。
- HSET：约 5.1 万 QPS，p50 约 1.671 ms。
- SADD：约 5.2 万 QPS，p50 约 1.623 ms。
- ZADD：约 2.5 万 QPS，p50 约 3.359 ms。

解释口径：

- 热点 SADD 数字高，是因为重复 key/member 更容易命中 `set_member_cache_`，新增少、底层写少，不能代表真实随机新增集合性能。
- 随机 GET 命中率不明确，可能大量 miss，因此不能简单当作随机命中读结论。
- ZADD 慢于 SADD/HSET，因为要维护 elem->score、score->elem、size，分数变化还要写旧 score tombstone。
- 数据能证明：在给定 VMware、100 并发、10 万请求条件下，热点读和部分随机写有可观吞吐。
- 数据不能证明：完整 Redis 兼容、工业级事务、长期运行稳定性、pipeline 性能、fsync 强一致性能。

## 7. 模拟面试：连续追问版

**Q1：介绍一下 TinyLSM。**  
我的回答：这是一个 C++20 单机持久化 KV，引擎用 LSM-Tree，上层扩展部分 Redis RESP 命令。写入 WAL-first，进入 SkipList MemTable，flush 成 SST，后台 Compaction 合并；上层用前缀键映射 Hash/Set/ZSet，并做成员索引缓存。

更好的回答：加一句“这是教学型实现，不宣称完整 Redis”，同时给出 benchmark 数字和测试覆盖。

易踩坑：说成“完整 Redis”。

**Q2：SET 从客户端到落盘怎么走？**  
我的回答：`RedisSession::do_read` 读 RESP，`handleRequest` 解析，`set_handler` 调 `RedisWrapper::redis_set`，再到 `LSM::put_batch`。`LSM::put_batch` 先写 WAL Record，再写 MemTable；满了冻结并后台 flush 到 SST。

追问：为什么用 `put_batch`？  
更好的回答：SET 需要同时写业务 key 和类型 key，batch 能保证这组内部 KV 在一条自动提交链路中写入。

易踩坑：漏掉类型 key。

**Q3：WAL 恢复怎么避免重放未提交事务？**  
我的回答：`WAL::recover` 读取 checkpoint 后的 WAL，按事务 id 分组，只有出现 `OP_COMMIT` 的事务留在结果 map，`LSM::LSM` 只重放这些记录。

追问：checkpoint 哪里来？  
更好的回答：`TranManager::get_checkpoint_tranc_id` 基于 flushed transaction set，flush 完成后 `add_flushed_tranc_ids` 更新 WAL checkpoint。

易踩坑：说成所有 WAL 都重放。

**Q4：MemTable 为什么要 Frozen？**  
我的回答：当前表满了不能阻塞新写，所以把旧表变成 Frozen，新写进入新 SkipList，后台慢慢 flush Frozen。

追问：读请求怎么保证不丢？  
更好的回答：`MemTable::get` 查 current 和 frozen；flush 发布 SST 后再移除 frozen。

易踩坑：说 flush 时直接清空内存。

**Q5：SST 点查怎么减少磁盘 I/O？**  
我的回答：先判断 key 是否在 SST range，然后 Bloom 过滤不存在 key，定位 block 后读 BlockCache，cache miss 才读文件。

追问：Bloom miss 能直接返回吗？  
更好的回答：能，Bloom 不会 false negative；Bloom hit 仍可能 false positive，需要继续查 block。

易踩坑：说 Bloom 能判断一定存在。

**Q6：Compaction 怎么处理 MVCC 版本？**  
我的回答：`gen_sst_from_iter` 用最老活跃事务 id 作为 watermark，保留所有比 watermark 新的版本，对 watermark 之前每个 key 保留最新一条，包括 Tombstone。

追问：为什么 Tombstone 也保留？  
更好的回答：它可能还需要屏蔽下层旧值，过早删除会让已删除 key 复活。

易踩坑：说删除标记可以在下一次 compact 立即删。

**Q7：RU/RC/RR 分别怎么实现？**  
我的回答：RU 直接写 MemTable 并保存 rollback；RC/RR 写 temp_map，commit 时写 WAL 和 MemTable；RR 把第一次读结果放 read_map 保证重复读。

追问：这是不是完整隔离？  
更好的回答：不是，只是教学型隔离路径，缺少 start/read/commit timestamp 的完整模型。

易踩坑：宣称 Serializable。

**Q8：WiscKey 在哪里做 value 转换？**  
我的回答：在 Engine 的 `tran_vlog` 和 `tran_vlog_batch` 中，把大 value append 到 VLog，返回 12 字节引用；读路径统一由 `resolve_value_try` 还原。

追问：Compaction 会重新写 VLog 吗？  
更好的回答：当前不重新追加，Compaction 复用引用，这减少写放大；但 VLog GC 未实现。

易踩坑：把 SSTBuilder 里注释掉的分离逻辑说成仍在使用。

**Q9：Redis ZSet 为什么要两套 key？**  
我的回答：member 查 score 需要 `ELEM_`；按 score 排序范围查询需要 `SCORE_`。所以 ZADD 同时写两条索引。

追问：同 score 怎么处理？  
更好的回答：score key 后拼 elem，避免同 score 覆盖。

易踩坑：说只靠一个 map 就能支持所有查询。

**Q10：benchmark 为什么热点和随机差这么多？**  
我的回答：热点重复访问少量 key/member，RedisWrapper 成员缓存和 BlockCache 命中高；随机空间新增多、缓存命中低、写放大更多，ZADD 还要维护双索引。

追问：你会怎么继续优化？  
更好的回答：按 key 分片锁、优化 ZSet score 编码、批量命令减少 WAL sync/锁开销、改进 Compaction 调度、实现 VLog GC 和更精确的 benchmark。

易踩坑：只报最高 QPS，不说明负载。

