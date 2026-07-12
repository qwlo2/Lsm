# TinyLSM 源码核验版面试讲稿

本文档基于当前工作区 `tiny-lsm` 源码核验，不修改源码、不修改简历。源码中的命名仍大量使用 `tranc_id`，因此下文除解释语义外，不把它强行改称为独立的 `commit_id/read_id/start_id`。

## 错误与修正表

| 草稿原文 | 源码结论 | 是否正确 | 推荐表述 | 文件/类/函数证据 |
| ---- | ---- | ---- | ---- | --------- |
| 实现了 WiscKey 的大小值分离 | 已实现 VLog 写入、批量写入、SST 中存 12 字节引用、读取时 resolve；GC 未实现 | 基本正确 | 实现了教学型 WiscKey 大小值分离，VLog GC 尚未实现 | `include/vlog/vlog.h` `VLog`；`src/lsm/engine.cpp` `LSMEngine::tran_vlog`、`resolve_value_try` |
| 大 Value 超过的 v 会写入 vlog | 阈值来自 `WISCKEY_VALUE_THRESHOLD`，当前默认/配置为 12；代码限制阈值不能大于 12 | 基本正确 | 大 value 是否分离由 `TomlConfig::getWisckeyValueThreshold()` 控制，当前配置为 12，且要求 `value.size() >= threshold` | `config.toml` `[lsm.wisckey]`；`src/config/config.cpp`；`src/lsm/engine.cpp` `tran_vlog` |
| 12 字节是分离阈值 | 12 字节首先是 ValuePointer 长度：`uint64_t offset + uint32_t size`；当前配置恰好把阈值设为 12 | 部分正确 | 12 字节是引用长度；当前项目把分离阈值配置为 12，但不能把“引用长度”和“阈值概念”混为一谈 | `include/vlog/vlog.h` 注释；`src/lsm/engine.cpp` `tran_vlog` |
| 自动提交 put：获取事务 id，盲写不做写冲突检测 | 当前 `LSM::put` 获取事务 id 后没有调用 `chech_write`，直接走 VLog、WAL、MemTable | 正确 | 自动提交 put 视为盲写，不做乐观写冲突检测 | `src/lsm/engine.cpp` `LSM::put` |
| 自动提交 put 写入事务开始、结束和增删 Record | 当前自动提交 `put` 只写 `putRecord` 和 `commitRecord`；`createRecord` 被注释 | 不正确 | 自动提交 put 只写 `OP_PUT + OP_COMMIT`；显式事务在 `new_tranc` 时写 `OP_CREATE` 到事务记录区 | `src/lsm/engine.cpp` `LSM::put`；`src/lsm/transation.cpp` `TranManager::new_tranc` |
| WAL Record 生成 5 种：事务开始、结束、回滚、kv 增删 | `OperationType` 的确有 `OP_CREATE/OP_COMMIT/OP_ROLLBACK/OP_PUT/OP_DELETE`；静态函数名分别为 `createRecord/commitRecord/rollbackRecord/putRecord/deleteRecord` | 正确 | Record 支持 5 类操作类型，但不同路径实际写入的 Record 不完全相同 | `include/wal/record.h` `OperationType`、`Record` |
| 显式事务对象包括事务 id、隔离级别、临时区、回滚区、读缓存区、记录区 | 字段对应 `tranc_id_`、`isolation_level_`、`temp_map_`、`rollback_map_`、`read_map_`、`operations` | 正确 | 显式事务上下文保存隔离级别、暂存写集、RU 回滚信息、RR 读缓存和 WAL Record 列表 | `include/lsm/transaction.h` `TranContext` |
| RU put 不冲突则把转换后的 kv 写入事务回滚区 | RU 实际把转换后的新 value 写入 MemTable；回滚区保存旧值或空状态 | 不正确 | RU put 先 VLog 转换，冲突检测通过后立即写 MemTable；`rollback_map_` 只保存覆盖前旧值用于 abort | `src/lsm/transation.cpp` `TranContext::put` |
| RU put 冲突则 abort，并对写入到 MemTable 的 kv 回滚 | 当前冲突检测发生在写 MemTable 之前；若冲突直接 `abort()`，通常还没有本次写入 | 部分正确 | RU 写冲突会 abort；只有已经写入过的 RU 修改才依赖 `rollback_map_` 回滚 | `src/lsm/transation.cpp` `TranContext::put`、`abort` |
| RU get 直接查找 | RU `get` 先查 `temp_map_`，但 RU put 不写 temp；随后用 `txn=0` 调 `engine_->get`，可见最新版本 | 基本正确 | RU 读路径实际以 `txn=0` 读最新可见版本，因此可能读到其他未提交 RU 写入 | `src/lsm/transation.cpp` `TranContext::get` |
| RU commit 从回滚区增加事务开始和结束 record，写 WAL | RU commit 不使用 `rollback_map_` 构造 WAL；`operations` 已在 put/remove 时记录，commit 追加 `commitRecord` 后写 WAL | 不正确 | RU commit 写 `operations + OP_COMMIT`，回滚区只服务 abort，不参与 WAL 构造 | `src/lsm/transation.cpp` `TranContext::commit` |
| RC put 写临时区以及事务记录区 | RC 分支把 VLog 转换后的 value 写入 `temp_map_`，同时追加 `putRecord` 到 `operations` | 正确 | RC 写入先进入事务暂存区，不立即进入 MemTable | `src/lsm/transation.cpp` `TranContext::put` |
| RC get 先查临时区，再在 MemTable 查找 | 源码先查 `temp_map_`、再查 `read_map_`、再调用 `engine_->get(key, tranc_id_)`；`engine_->get` 会查 MemTable 和 SST | 部分正确 | RC get 支持 read-your-own-write，未命中后按当前事务 id 查询引擎 | `src/lsm/transation.cpp` `TranContext::get` |
| RC commit 写冲突检测后，将记录区写入 WAL；冲突 abort | 当前逻辑如此；WAL 在写 MemTable 前写入 | 正确 | RC commit 对 `temp_map_` 每个 key 做写冲突检测，成功后先写 WAL，再写 MemTable | `src/lsm/transation.cpp` `TranContext::commit` |
| RR put 和 RC 基本一致 | RR put 与 RC 一样写 `temp_map_` 和 `operations`，不立即写 MemTable | 正确 | RR 写路径与 RC 相同，读路径多了 `read_map_` 重复读缓存 | `src/lsm/transation.cpp` `TranContext::put/get` |
| RR get 通过读缓存实现重复读 | 当前第一次读后把 `engine_->get` 的结果放入 `read_map_`，后续同 key 直接返回缓存 | 正确 | RR 使用事务内 `read_map_` 做同 key 重复读缓存，是教学型 RR，不是完整快照系统 | `src/lsm/transation.cpp` `TranContext::get` |
| RC、RR 冲突时 abort，因为没有写 MemTable，只需等待其回收 | abort 会清空 `read_map_`、`temp_map_`、`operations`，并把事务 id 标记为 ABORTED ready；没有 MemTable 写入需要撤销 | 基本正确 | RC/RR abort 主要清理事务上下文；不会像 RU 一样根据 rollback_map 回滚 MemTable | `src/lsm/transation.cpp` `TranContext::abort` |
| 所有隔离级别 commit 或 abort 时会把 id 写入 ready-flush-id | commit 成功后调用 `add_ready_to_flush_tranc_id(... OP_COMMITTED)`；abort 调用 `add_ready_to_flush_tranc_id(... ABORTED)` | 正确 | active -> ready 表示事务已结束，但不代表已经落到 SST | `src/lsm/transation.cpp` `TranContext::commit/abort`、`TranManager::add_ready_to_flush_tranc_id` |
| flushed-id 的最小值用来清理 WAL | `flushedTrancIds_` 经 `compressSet` 后取 `begin()` 设置为 WAL checkpoint；cleaner 删除所有 record id 都 `<= checkpoint` 的旧 WAL 文件 | 基本正确 | WAL 清理依赖事务管理器设置的 checkpoint，而不是直接遍历所有 flushed id | `src/lsm/transation.cpp` `add_flushed_tranc_ids`；`src/wal/wal.cpp` `cleanWALFile` |
| active-id 的最小值用于 Compaction 清理 | `get_oldest_active_tranc_id()` 返回最小活跃事务 id；Flush 和 Compaction 都用它做版本保留 watermark | 正确 | 版本过滤以最老活跃事务 id 为可见性边界 | `src/lsm/transation.cpp` `get_oldest_active_tranc_id`；`src/lsm/engine.cpp` `gen_sst_from_iter`；`src/memtable/memtable.cpp` `flush_last` |
| 整个 put 流程先写入 MemTable，再超过数量冻结等待 flush | 自动提交源码顺序是 VLog 转换 -> WAL 写入 -> MemTable 写入 -> 插入事务结束标记 -> 可能冻结 -> 标记 ready -> 唤醒 flush | 不正确 | 自动提交写入遵循“VLog 引用生成在前，WAL-first 于 MemTable”的流程 | `src/lsm/engine.cpp` `LSM::put` |
| Flush 异步线程将一个 MemTable 刷成 SST | 当前 `flush_worker` 每次被唤醒调用 `flush_one_frozens()`，每次只处理一个 frozen table | 正确 | Flush worker 单次刷一个 frozen MemTable；`flush_all` 会循环刷完全部 | `src/lsm/engine.cpp` `flush_worker/flush_one_frozens/LSM::flush_all` |
| Flush 发布 SST 后移除 ready txn 到 flushed id | `memtable.flush_last` 返回 `flushed_tranc_ids`，发布 SST 到 L0 后删除 frozen table，再调用 `add_flushed_tranc_ids` | 正确 | Flush 完成 SST 构建和发布后，才把事务从 ready 推进到 flushed/checkpoint | `src/lsm/engine.cpp` `flush_one_frozens` |
| SST 中含有 block、block meta、bloom，还有 block-cache | SST 文件含 block data、meta section、Bloom section、footer；BlockCache 是内存对象，不在 SST 文件里 | 部分错误 | SST 文件持久化 block/meta/bloom/footer；BlockCache 是读路径共享的内存缓存 | `src/sst/sst.cpp` `SSTBuilder::build`、`SST::open` |
| BlockCache 采用 LRU-K | 代码有 `k_`、less-than-k 和 greater-equal-k 两个链表，按访问次数提升与淘汰；K 默认/配置为 8 | 基本正确 | 可说实现了 LRU-K 风格 Block Cache；不要夸大成工业级完整 LRU-K | `include/block/block_cache.h`；`src/block/block_cache.cpp`；`config.toml` |
| Bloom 通过多个 hash 位判断，若为 1 则可能存在，否则一定不存在 | `possibly_contains` 所有 hash 位为 true 返回 true，否则 false；hash 由 hash1/hash2/idx 组合生成 | 正确 | Bloom Filter 用于快速排除不存在 key，存在假阳性，不存在假阴性 | `src/utils/bloom_filter.cpp` |
| Compact 是异步线程，分 L0-L1 和其他层两条路径 | `compact_worker` 异步触发，但当前总是调用 `full_compact(0)`；内部递归到后续层 | 基本正确 | 当前是后台触发的简化 leveled full compaction：L0->L1 与 Lk->Lk+1 两条归并路径 | `src/lsm/engine.cpp` `compact_worker/full_compact/full_l0_l1_compact/full_common_compact` |
| 当前层超过阈值时选择部分 SST 与下一层重叠 SST 归并 | 当前实现取当前层全部 SST 和下一层全部 SST，而不是选择重叠范围 | 不正确 | 当前压缩是 full compact 风格：当前层全量 + 下一层全量归并 | `src/lsm/engine.cpp` `full_compact` |
| L0 用堆组织，L1 用层次迭代器，随后 TwoMergeIterator | 当前 L0 用 `SstIterator::merge_sst_iterator` 得到 heap；L1 用 `ConcactIterator`；再 `TwoMergeIterator` | 正确 | L0 重叠 SST 先堆归并，L1+ 利用有序 SST 串接迭代 | `src/lsm/engine.cpp` `full_l0_l1_compact` |
| 1 和其他合并 | 实际是 src_level 非 0 时，将 Lx 和 Ly 都构造成 `ConcactIterator`，用 `TwoMergeIterator` 合并 | 基本正确 | L1 及以上相邻层归并使用两个 `ConcactIterator` + `TwoMergeIterator` | `src/lsm/engine.cpp` `full_common_compact` |
| 只保留同 key 中 commit-id 大于最小 active-id 和第一个小于等于最小 active-id 的记录 | 当前字段仍叫事务 id/version id，不是独立 commit_id；规则本身在 `gen_sst_from_iter` 和 `flush_last` 中实现 | 部分正确 | 对同 key 保留所有 `txn_id > watermark` 的版本，并保留第一个 `txn_id <= watermark` 的版本；这里叫 transaction/version id 更准确 | `src/lsm/engine.cpp` `gen_sst_from_iter`；`src/memtable/memtable.cpp` `flush_last` |
| WAL 写入采取批量写入，也可以强制写入 | `WAL::log(records,false)` 追加缓冲，达到 buffer 或 force 才写文件并 sync；`flush()` 强制刷缓冲并 sync | 正确 | WAL 支持缓冲批量写入和强制 flush/sync | `src/wal/wal.cpp` `WAL::log/flush` |
| 崩溃恢复时读取所有 record，并筛选未 flushed 的 | `WAL::recover` 只保留 checkpoint 之后、且出现 `OP_COMMIT` 的事务记录；LSM 构造函数重放到 MemTable | 基本正确 | 恢复读取 WAL 中 checkpoint 后的已提交事务，跳过未提交事务，再重放 PUT/DELETE | `src/wal/wal.cpp` `recover`；`src/lsm/engine.cpp` `LSM::LSM` |
| VLog 实现单条或批量写入，GC 未实现 | `append`、`append_batch` 存在；头文件注释 `GC没做` | 正确 | VLog 已支持单条和批量追加；当前没有实现 VLog GC | `include/vlog/vlog.h`；`src/vlog/vlog.cpp` |
| Redis 基本类型基本命令兼容 | `RedisWrapper` 提供 String/Hash/List/Set/ZSet 的部分常用命令，不是完整 Redis | 基本正确 | 兼容部分 Redis RESP 命令和常用数据结构，不宣称完整 Redis | `include/redis_wrapper/redis_wrapper.h` |
| Set 缓存 member 集合和 size | `set_member_cache_` 与 `set_size_cache_` 存在；`SADD` 会懒加载并维护 | 正确但需补充 | Set 缓存 member 和 size；但当前 `SREM` 没有同步更新该缓存，面试里要谨慎 | `src/redis_wrapper/redis_wrapper.cpp` `load_set_cache_unlocked/redis_sadd/redis_srem` |
| ZSet 缓存 member -> score 映射和 size | `zset_elem_score_cache_` 与 `zset_size_cache_` 存在，`ZADD/ZREM/ZCARD/ZSCORE` 使用 | 正确 | ZSet 缓存 elem->score 和 size，score 排序索引仍通过编码 key 支持范围扫描 | `include/redis_wrapper/redis_wrapper.h`；`src/redis_wrapper/redis_wrapper.cpp` |
| Hash 缓存 field 集合和 size | `hash_field_cache_` 与 `hash_size_cache_` 存在；不缓存 field value | 正确 | Hash 缓存 field 集合和 size，field value 仍存于 LSM | `src/redis_wrapper/redis_wrapper.cpp` `load_hash_cache_unlocked/redis_hset_batch` |
| 加入 key-type 判断是否同类型，string 会强制改变 | `check_or_create_type_unlocked` 做统一命名空间检查；`redis_set` 直接写 string 类型并清缓存 | 基本正确 | 非 String 写路径检查类型；String SET 是特殊覆盖路径，会更新类型并清理进程缓存 | `src/redis_wrapper/redis_wrapper.cpp` `check_or_create_type_unlocked/redis_set` |

## 30 秒项目介绍

TinyLSM 是一个 C++20 实现的单机持久化 KV 存储系统，核心是 LSM-Tree：写入先经过 WAL，再进入 SkipList MemTable，冻结后由后台 Flush 线程生成 SST；读路径会合并 MemTable、Frozen MemTable 和多层 SST，并利用 Bloom Filter 与 Block Cache 减少磁盘访问。在这个存储内核上，项目扩展了部分 Redis RESP 命令，把 String、Hash、Set、List、ZSet 编码为内部 KV，同时为 Hash/Set/ZSet 做了进程内成员索引缓存。事务方面实现了教学型 RU、RC、RR 路径、WAL 恢复、Checkpoint 和基于最老活跃事务 id 的版本保留。

## 1 分钟项目介绍

这个项目可以分成三层：底层是 LSM 存储引擎，中间是事务和恢复，上层是 Redis 兼容层。写入时，自动提交 `put` 会先根据 WiscKey 阈值把大 value 写入 VLog 并生成 12 字节引用，然后写 WAL Record，最后写入 MemTable。MemTable 达到大小限制后被冻结，Flush worker 每次把一个 frozen table 刷成 SST，并在 SST 发布后把对应事务 id 推进到 flushed 集合，用于 WAL checkpoint。读路径会先查 MemTable/Frozen，再查 SST；SST 上有 Bloom Filter 做快速排除，BlockCache 缓存热点 block。

事务上，显式事务由 `TranContext` 保存临时写集、RU 回滚信息、RR 读缓存和 WAL Record。RU 会立即写 MemTable，所以可能出现脏读；RC/RR 的写先暂存在 `temp_map_`，commit 时做写冲突检测、写 WAL，再写 MemTable；RR 额外用 `read_map_` 保证同 key 重复读。Redis 层用类型 key 保证统一命名空间，将 Hash field、Set member、ZSet elem/score 编码成前缀 key，并通过懒加载缓存减少热点成员操作的重复前缀扫描。

## 3 分钟项目介绍

TinyLSM 的主链路是 WAL-first 的 LSM-Tree。自动提交写入由 `LSM::put` 进入，先获取 `tranc_id`，再根据 `WISCKEY_VALUE_THRESHOLD` 判断是否写 VLog。若 value 需要分离，`LSMEngine::tran_vlog` 会把原始 value 追加到 `vlog.data`，MemTable 和 WAL 中保存 `[offset:uint64][size:uint32]` 这样的 12 字节引用。随后构造 `OP_PUT` 和 `OP_COMMIT` 两条 WAL Record，调用 `TranManager::write_to_wal` 进入 `WAL::log`。`WAL::log` 不是每次都立即写盘，而是先进入缓冲区；达到 buffer 阈值或 force flush 时才编码写文件并 sync。WAL 写入成功后，数据才进入 MemTable，并插入空 key/value 作为该事务已经进入 MemTable 的标记。

MemTable 使用 SkipList 保存多版本数据。当前表超过配置大小后移动到 frozen list，Flush worker 被条件变量唤醒，每次从最老 frozen table 构造一个 SST。Flush 时会扫描 SkipList，跳过空 key/value 的事务结束标记并收集 flushed transaction ids，同时根据最老活跃事务 id 做版本过滤：同 key 中保留所有大于 watermark 的版本，再保留第一个小于等于 watermark 的版本。SST 文件本身包含 block data、block meta、Bloom section 和 footer；BlockCache 是内存缓存，不是 SST 文件内容。SST 发布到 L0 后才移除对应 frozen table，并把 flushed ids 交给 `TranManager` 更新 checkpoint。

Compaction 是后台线程触发的简化 leveled full compaction。当前 worker 总是从 `full_compact(0)` 开始，L0 超阈值时把 L0 全部 SST 和 L1 全部 SST 合并；L0 因为 key range 可重叠，所以先用堆迭代器归并，L1 以上的有序层使用 `ConcactIterator` 串接，再由 `TwoMergeIterator` 做二路归并。L1 及以上层之间则使用两个 `ConcactIterator` 归并。最终仍在 `gen_sst_from_iter` 中用 watermark 做多版本和 Tombstone 保留。

事务实现是教学型 RU、RC、RR。RU 写入立即进入 MemTable，并用 `rollback_map_` 保存旧值用于 abort，因此其他 RU 读可能通过 `txn=0` 读到未提交数据；RC/RR 写入暂存到 `temp_map_`，commit 时对所有写 key 调 `LSMEngine::chech_write`，检查是否已有更新事务 id 的版本，成功后先写 WAL，再刷入 MemTable。RR 在读路径多了 `read_map_`，第一次读到的结果会缓存，之后同 key 重复读返回缓存值。

RedisWrapper 层把 Redis 命令映射到内部 KV。比如 Hash field 用 field 前缀 key，Set member 用 set 前缀 key，ZSet 同时维护 elem->score key 和 score->elem key，支持基于前缀扫描的查询。为了避免热点命令反复扫描，Hash 缓存 field 集合和 size，Set 缓存 member 集合和 size，ZSet 缓存 elem->score 和 size；这些缓存都是进程内的，首次访问通过 LSM 前缀扫描懒加载，写入时增量维护，删除/过期/SET 覆盖时部分路径会清理或更新缓存。

## 自动提交 put 完整调用链

1. `LSM::put(key, value)` 获取 `tranc_id = TranManager::getNextTransactionId()`。
2. 读取 `TomlConfig::getWisckeyValueThreshold()`；如果 `engine->vlog_` 存在、阈值大于 0 且 `value.size() >= threshold`，调用 `LSMEngine::tran_vlog(key, value)`。
3. `tran_vlog` 将原 value 写入 `VLog::append`，返回 12 字节引用 `[offset:uint64][value_size:uint32]`；小 value 直接返回原 value。
4. 构造 WAL Record：当前自动提交 put 只构造 `Record::putRecord(tranc_id, key, value_)` 和 `Record::commitRecord(tranc_id)`，不写 `createRecord`。
5. `TranManager::write_to_wal` 调 `WAL::log(records, false)`，WAL 可能只进入缓冲；达到阈值或 force 时写文件并 sync。
6. WAL 调用成功后，加 MemTable 锁，执行 `memtable.put_(key, value_, tranc_id)`。
7. 写入 `memtable.put_("", "", tranc_id)` 作为事务结束/Flush 识别标记。
8. 若 `current_table->get_size()` 超过 `LSM_PER_MEM_SIZE_LIMIT`，调用 `memtable.frozen_cur_table_()`，随后 `request_flush()`。
9. 调用 `TranManager::add_ready_to_flush_tranc_id(tranc_id, OP_COMMITTED)`，事务从 active 外部状态进入 ready-to-flush。

注意：自动提交 put 当前不做写冲突检测；这是 blind write 路径。WAL 语义是 WAL before MemTable，但 VLog 引用生成发生在 WAL 前。

## RU 的 put/get/commit/abort

**put**

- 数据先经 `tran_vlog` 做大 value 分离。
- 如果隔离级别是 `READ_UNOP_COMMITTED`，加 SST、MemTable current、frozen 锁。
- 先用 `memtable.get_(key, tranc_id_)` 读取旧值，用于回滚。
- 调 `engine_->chech_write(key, tranc_id_)` 检查是否存在更新事务 id 的同 key 版本。
- 无冲突后立即 `memtable.put_(key, value_, tranc_id_)`。
- `rollback_map_` 保存旧值：旧值为空则存 `nullopt`，否则保存旧 value 和旧 txn id。
- `operations` 追加 `Record::putRecord(tranc_id_, key, value_)`。

**get**

- 先查 `temp_map_`，但 RU put 不写 temp，所以通常无命中。
- 再查 `read_map_`，RU 不写 read cache。
- 使用 `txn=0` 调 `engine_->get(key, 0)`，读取最新版本，可能读到其他未提交 RU 写入。

**commit**

- RU 不遍历 `temp_map_` 做写冲突检测，因为 RU 已经在 put 阶段检测并写入 MemTable。
- 追加 `Record::commitRecord(tranc_id_)`。
- 调 `write_to_wal(operations)`。
- 不再从 `temp_map_` 写 MemTable。
- 插入空 key/value 的事务结束标记，必要时冻结 MemTable。
- 调 `add_ready_to_flush_tranc_id(tranc_id_, OP_COMMITTED)`。

**abort**

- 清空 `read_map_`、`temp_map_`。
- RU 根据 `rollback_map_` 撤销已经写入 MemTable 的内容：旧值不存在则 `memtable.remove_`，旧值存在则 `memtable.put_` 回旧值。
- 清空 `rollback_map_` 和 `operations`。
- 标记 `isAborted=true`，调用 `add_ready_to_flush_tranc_id(tranc_id_, ABORTED)`。

## RC 的 put/get/commit/abort

**put**

- 先做 VLog 转换，得到原 value 或 12 字节引用。
- 写入 `temp_map_[key] = value_`。
- `operations` 追加 `putRecord`。
- 不立即进入 MemTable；不在 put 阶段做写冲突检测。

**get**

- 先查 `temp_map_`，支持 read-your-own-write；若 value 为空串表示删除，返回空。
- 再查 `read_map_`，但 RC 不写 read cache。
- 使用 `txn=tranc_id_` 调 `engine_->get(key, tranc_id_)`。

**commit**

- 对 `temp_map_` 中每个 key 调 `engine_->chech_write(key, tranc_id_)`。
- 冲突则 `abort()` 并返回 false。
- 无冲突则追加 `commitRecord`，先写 WAL。
- WAL 成功后，将 `temp_map_` 中每个 key 写入 MemTable。
- 插入空 key/value 事务结束标记，必要时冻结。
- 调 `add_ready_to_flush_tranc_id(tranc_id_, OP_COMMITTED)`。

**abort**

- 因为 RC 没有提前写 MemTable，所以 abort 只需要清理 `temp_map_`、`read_map_`、`operations`。
- 调 `add_ready_to_flush_tranc_id(tranc_id_, ABORTED)`。

## RR 的 put/get/commit/abort

RR 的写路径与 RC 基本一致；差别在读路径。

**put**

- VLog 转换后写 `temp_map_`。
- 追加 `putRecord` 到 `operations`。
- 不立即进入 MemTable。

**get**

- 先查 `temp_map_`，保证读到自己的写。
- 再查 `read_map_`；如果之前读过同一个 key，直接返回缓存结果。
- 未命中时使用 `txn=tranc_id_` 调 `engine_->get`。
- 第一次读取结果写入 `read_map_[key]`，后续同 key 重复读固定返回该结果。

**commit**

- 与 RC 一致：对 `temp_map_` 做写冲突检测，成功后写 WAL，再写 MemTable，最后标记 ready。

**abort**

- 与 RC 一致：清理临时写集、读缓存和 WAL Record 列表，标记 ABORTED ready。

**边界**

- 当前 RR 是基于事务内 read cache 的教学型重复读，不是完整工业 MVCC 快照。
- 当前项目仍使用单一递增 `tranc_id` 作为版本号，没有独立 start/read/commit timestamp。

## Flush 完整调用链

1. 写路径在 MemTable 超过 `LSM_PER_MEM_SIZE_LIMIT` 时调用 `memtable.frozen_cur_table_()`，把 current SkipList 放入 `frozen_tables` 头部，并新建 current table。
2. 写路径随后调用 `LSMEngine::request_flush()`，设置 `flush_requested_ = true` 并通知条件变量。
3. `flush_worker()` 被唤醒后清除 flag，调用 `flush_one_frozens()`。
4. `flush_one_frozens()` 先检查是否有 frozen table；没有则返回。
5. 分配新的 SST id，按是否启用 WiscKey 构造 `SSTBuilder`。
6. 读取 watermark：`TranManager::get_oldest_active_tranc_id()`；无活跃事务时返回下一个事务 id。
7. 调 `MemTable::flush_last(builder, path, sst_id, flushed_tranc_ids, block_cache, flushed_table, watermark)`。
8. `flush_last` 选择最老 frozen table，但不立即从 `frozen_tables` 删除；构建 SST 时过滤版本并收集空 key/value 标记里的 flushed txn ids。
9. SST 构建完成后，`flush_one_frozens` 在 `ssts_mtx` 下把 SST 发布到 `ssts` 和 `level_sst_ids[0]`。
10. 发布后调用 `memtable.remove_flushed_table_(flushed_table)` 删除 frozen table。
11. 调 `TranManager::add_flushed_tranc_ids(flushed_tranc_ids)` 推进 checkpoint。
12. 如果 L0 SST 数量达到 `LSM_SST_LEVEL_RATIO`，调用 `request_compact(0)`。

## Compaction 完整调用链

1. `request_compact(level)` 只设置 `compact_requested_ = true` 并唤醒后台线程；当前没有保存传入 level。
2. `compact_worker()` 被唤醒后总是调用 `full_compact(0)`。
3. `full_compact(src_level)` 检查当前层 SST 数量是否达到 `ratio * (src_level + 1)`。
4. 达到阈值后，复制当前层和下一层的全部 SST id 与 shared_ptr。
5. 如果 `src_level == 0`，调用 `full_l0_l1_compact(l0_ssts, l1_ssts)`。
6. L0->L1 中，L0 用 `SstIterator::merge_sst_iterator` 构造 heap，L1 用 `ConcactIterator`，再用 `TwoMergeIterator` 合并。
7. 如果 `src_level > 0`，调用 `full_common_compact(lx_ssts, ly_ssts, src_level + 1)`。
8. L1+ 合并中，两边都用 `ConcactIterator`，再用 `TwoMergeIterator` 合并。
9. 归并结果进入 `gen_sst_from_iter`，按最老活跃事务 id 做版本过滤。
10. 生成新 SST 后，在 `ssts_mtx` 下删除旧 SST 文件和索引，将新 SST 放入下一层。
11. 如果下一层也达到阈值，递归调用 `full_compact(src_level + 1)`。

**版本过滤规则**

- 对同一个 key，所有 `txn_id > oldest_active_txn` 的版本都保留。
- 对 `txn_id <= oldest_active_txn` 的版本，只保留遇到的第一个版本。
- Tombstone 在当前实现中是空 value，按同样版本规则保留或丢弃。

**推荐命名**

- 可以说“简化的 leveled full compaction”。
- 不要说“按重叠 key range 选择部分 SST”，当前源码是当前层全部 SST + 下一层全部 SST。

## WAL 清理与恢复完整调用链

**写入**

1. 自动提交或显式事务构造 `Record` 列表。
2. `TranManager::write_to_wal(records)` 调 `WAL::log(records, false)`。
3. `WAL::log` 将 records 放入 `log_buffer_`。
4. 如果 buffer 未达到 `buffer_size_` 且不是 force flush，直接返回。
5. 达到条件后，对缓冲内所有 Record 调 `encode()`，追加到当前 WAL 文件。
6. 调 `log_file_.sync()`。
7. 文件大小达到 `file_size_limit_` 后 `reset_file()` 滚动到新 WAL 文件。

**Checkpoint 与清理**

1. 显式事务 commit/abort 后从 `activeTrans_` 移入 `readyToFlushTrancIds_`；自动提交事务没有进入 `activeTrans_`，写完 MemTable 后直接登记 ready。
2. Flush 收集真正进入 SST 的事务结束标记，调用 `add_flushed_tranc_ids(ids)`。
3. `add_flushed_tranc_ids` 将已刷入的 committed 事务和满足条件的 aborted 事务推进到 `flushedTrancIds_`，并调用 `compressSet`。
4. 当前 checkpoint 取 `flushedTrancIds_.begin()`，再调用 `wal->set_checkpoint_tranc_id(checkpoint)`。
5. `WAL::cleaner` 周期调用 `cleanWALFile()`。
6. `cleanWALFile` 遍历旧 WAL 文件，跳过最后一个活动文件；如果文件里所有 Record 的 `tranc_id <= checkpoint_tranc_id_`，删除该文件。

**恢复**

1. `LSM::LSM(path)` 初始化 engine 和 tran_manager 后调用 `tran_manager_->check_recover()`。
2. `check_recover()` 调 `WAL::recover(data_dir_, checkpoint)`。
3. `recover` 找到所有 `wal.*` 文件，按序号升序读取，`Record::decode` 解码。
4. 只保留 `tranc_id > checkpoint` 的记录。
5. 只保留出现 `OP_COMMIT` 的事务；未提交事务被丢弃。
6. `LSM::LSM` 遍历恢复记录：`OP_PUT` 重放为 `memtable.put_`，`OP_DELETE` 重放为 `memtable.remove_`。
7. 每个恢复事务补写空 key/value 标记，并调用 `add_ready_to_flush_tranc_id(... OP_COMMITTED)`。
8. 最后 `init_new_wal()` 打开新的 WAL 文件，后续写入继续追加。

## Redis Hash/Set/ZSet 缓存结构

**统一命名空间**

- 类型元数据 key：`REDIS_TYPE_ + key`。
- 非 String 写路径通过 `check_or_create_type_unlocked(key, type)` 判断类型是否匹配。
- 如果类型 key 不存在，会写入类型；如果存在且不同，返回 `WRONGTYPE`。
- `redis_set` 是特殊覆盖路径：直接写普通 key 和 string 类型 key，然后清理该 key 的进程内缓存。

**Hash**

- field key：`get_hash_field_prefix(key) + field`。
- 缓存结构：`hash_field_cache_[key]` 保存 field 集合；`hash_size_cache_[key]` 保存 field 数量；`hash_loaded_` 标记是否已加载。
- 首次加载：`load_hash_cache_unlocked` 通过前缀扫描 field key，填充 field 集合和 size。
- `HSET`：懒加载后插入新 field，新增时 size++，field value 写入 LSM。
- `HGET`：直接查 field key，不从 field 缓存取 value。
- `HDEL`：懒加载 field 集合，删除 field 并维护 size；size 为 0 时删除 size/type/expire 并清缓存。

**Set**

- member key：`get_set_member_prefix(key) + member`，value 固定为 `"1"`。
- size key 当前用 `get_set_key_preffix(key)`。
- 缓存结构：`set_member_cache_[key]` 保存 member 集合；`set_size_cache_[key]` 保存数量；`set_member_loaded_` 标记是否已加载。
- 首次加载：`load_set_cache_unlocked` 通过前缀扫描 member key，填充 member 集合和 size。
- `SADD`：懒加载后只写新增 member，并维护 size。
- `SREM`：当前源码主要通过 LSM 查询和 `remove_batch` 删除，没有同步维护 `set_member_cache_`；如果缓存已加载，后续 `SADD` 有潜在陈旧缓存风险。面试中不要说 Set 删除路径已经完整维护缓存。
- `SISMEMBER/SCARD/SMEMBERS`：当前主要走 LSM 查询或前缀扫描，不完全依赖 Set 成员缓存。

**ZSet**

- elem key：`get_zset_key_elem(key, elem)`，value 为 score。
- score key：`get_zset_key_socre(key, score, elem)`，value 为 elem；同分数通过拼接 elem 区分。
- size key：`get_zset_key_preffix(key)`。
- 缓存结构：`zset_elem_score_cache_[key][elem] = score`；`zset_size_cache_[key]` 保存数量；`zset_loaded_` 标记是否已加载。
- 首次加载：`load_zset_cache_unlocked` 扫描 elem 前缀，构造 elem->score 映射和 size。
- `ZADD`：懒加载后新增 elem 时 size++；score 变化时删除旧 score key，更新 elem->score，再写新 elem key 和 score key。
- `ZREM`：懒加载后删除 elem key 和 score key，并维护 size；size 为 0 时删除 type/expire/size 并清缓存。
- `ZCARD/ZSCORE`：使用 zset 缓存快速返回。
- `ZRANGE` 等按 score 顺序的命令仍依赖 score key 编码和前缀扫描。

**缓存性质**

- 所有这些缓存都是进程内结构，不持久化。
- 进程重启后缓存为空，首次访问通过前缀扫描重建。
- `delete_key_unlocked` 和 `erase_key_cache_unlocked` 用于删除 key 或类型覆盖时清理缓存。

## 面试中不能说的错误表述

1. 不能说“12 字节就是 WiscKey 分离阈值”。应说 12 字节是 ValuePointer 长度，当前配置阈值也为 12。
2. 不能说“自动提交 put 写 BEGIN/PUT/COMMIT”。源码中自动提交 put 当前只写 `OP_PUT` 和 `OP_COMMIT`。
3. 不能说“显式事务 commit 时才做 VLog 转换”。源码中显式事务 `put` 阶段已经转换并放入 `temp_map_` 或 MemTable。
4. 不能说“RU commit 从 rollback_map 构造 WAL”。rollback_map 只用于 abort 回滚。
5. 不能说“RC/RR 也写 rollback_map”。源码中只有 RU 分支写 rollback_map。
6. 不能说“自动提交 remove 写 `OP_DELETE`”。当前自动提交 remove/remove_batch 用空 value 的 `putRecord` 表示删除；显式事务 remove 才写 `deleteRecord`。
7. 不能说“WAL::log 每次都会 sync”。只有缓冲达到阈值或 force flush 时才写文件并 sync。
8. 不能说“WAL cleaner 直接用最大 flushed id 删除”。源码使用 checkpoint，并扫描旧 WAL 文件 record id。
9. 不能说“BlockCache 是 SST 文件的一部分”。它是内存缓存。
10. 不能说“Bloom 返回 true 表示一定存在”。Bloom true 只是可能存在。
11. 不能说“当前 Compaction 选择与下一层 key range 重叠的部分 SST”。当前是当前层全量 + 下一层全量。
12. 不能说“后台 Compaction 会按 request 的 level 执行”。当前 worker 总是从 `full_compact(0)` 开始。
13. 不能说“SST 里的版本字段就是 commit_id”。当前源码还是 `tranc_id`，更稳妥叫 transaction/version id。
14. 不能说“实现工业级 MVCC/Serializable”。当前是教学型 RU/RC/RR 路径。
15. 不能说“VLog GC 已实现”。源码明确没有 GC。
16. 不能说“Hash 缓存 field-value”。Hash 缓存 field 集合和 size，value 仍查 LSM。
17. 不能说“Set 删除完整维护成员缓存”。当前 `SREM` 没有同步更新 set cache。
18. 不能说“缓存持久化”。缓存只在进程内。
19. 不能说“实现完整 Redis”。只能说兼容部分 RESP 命令和常用数据结构。
20. 不能说“String SET 会先完整删除旧复杂类型的所有内部 key”。当前 `redis_set` 清缓存并写类型，但 `delete_key_unlocked(key)` 被注释，源码不保证清理旧复杂类型所有内部 key。

## 最可能被连续追问的 20 个问题

1. **为什么 WiscKey 要把大 value 放到 VLog？**
   减少 SST 和 Compaction 搬运的大 value 数据量。当前项目的 Compaction 主要搬运 12 字节 ValuePointer，而不是重新追加 VLog。

2. **当前项目里 12 字节到底是什么？**
   是 ValuePointer 长度：8 字节 offset + 4 字节 value size。当前阈值配置也设为 12，但两者语义不同。

3. **自动提交 put 的真实顺序是什么？**
   获取事务 id -> VLog 转换 -> 构造 `OP_PUT/OP_COMMIT` -> WAL log -> MemTable put -> 插入事务结束标记 -> 可能 freeze -> ready。

4. **WAL-first 和 VLog-first 是否冲突？**
   这里 WAL-first 指 WAL 先于 MemTable；大 value 需要先写 VLog 生成引用，否则 WAL 无法记录最终写入 MemTable 的 value 引用。

5. **显式事务为什么 RC/RR 不立即写 MemTable？**
   因为 RC/RR 的写先进入 `temp_map_`，commit 时统一做写冲突检测，成功后再写 WAL 和 MemTable，避免 abort 时回滚 MemTable。

6. **RU 为什么可能脏读？**
   RU put 立即写 MemTable，RU get 用 `txn=0` 读最新版本，所以其他事务可能看到未提交写。

7. **RR 是怎么实现重复读的？**
   通过 `read_map_` 缓存第一次读取同 key 的结果；后续同 key 直接返回缓存。

8. **写冲突检测在哪里？**
   `LSMEngine::chech_write`，检查 MemTable/SST 中是否存在同 key 且事务 id 大于当前事务 id 的版本。

9. **rollback_map 保存什么？**
   只在 RU 中保存写入前的旧值或 `nullopt`，用于 abort 撤销已经写入 MemTable 的修改。

10. **operations 保存什么？**
    保存该事务要写 WAL 的 Record，包括 `OP_CREATE`、`OP_PUT/OP_DELETE`，commit 时追加 `OP_COMMIT`。

11. **readyToFlushTrancIds_ 的用途是什么？**
    表示事务已经 commit/abort 结束，但其提交标记还未确认随 MemTable flush 到 SST；Flush 后再转入 flushed/checkpoint。

12. **flushedTrancIds_ 的用途是什么？**
    表示已经安全刷入 SST 的事务边界集合，经 `compressSet` 后用于计算 WAL checkpoint。

13. **activeTrans_ 的用途是什么？**
    记录未结束的显式事务；其最小 id 用作 Flush/Compaction 的版本保留 watermark。

14. **WAL 恢复会不会重放未提交事务？**
    不会。`WAL::recover` 只保留出现 `OP_COMMIT` 的事务，且事务 id 必须大于 checkpoint。

15. **Flush 为什么要插入空 key/value？**
    当前实现用 `("", "", tranc_id)` 作为事务结束标记，Flush 时收集这些 id 来推进 flushed/checkpoint。

16. **SST 文件里有哪些部分？**
    Block data、BlockMeta section、Bloom Filter section、footer；WiscKey SST 额外写 storage mode 和 magic。

17. **BlockCache 是怎么工作的？**
    按 `(sst_id, block_id)` 缓存 Block，配置容量和 K；访问次数小于 K 和大于等于 K 的项分两条 list 管理，命中后提升。

18. **Compaction 为什么需要 watermark？**
    防止仍有活跃事务可能读到的历史版本被清理；watermark 之后的版本都保留，watermark 之前保留一个基准版本。

19. **RedisWrapper 为什么要类型 key？**
    Redis 是统一命名空间，同一个 key 不能同时是 hash/set/zset。类型 key 用来检查 `WRONGTYPE` 和支持 DEL/过期清理内部编码 key。

20. **Hash/Set/ZSet 缓存解决了什么问题？**
    避免热点命令反复进行 LSM 前缀扫描或点查。Hash 缓存 field，Set 缓存 member，ZSet 缓存 elem->score 和 size；但这些缓存只在进程内，且 Set 删除路径当前还不完整维护缓存。

## 推荐的源码验证版口述稿

我这个项目是一个 C++20 写的单机持久化 KV 存储系统，核心存储引擎是 LSM-Tree。写入路径先写 WAL，再进入 SkipList MemTable；MemTable 达到大小限制后会冻结成 Immutable MemTable，由后台 Flush 线程生成 SST。SST 文件中包含 block、block meta、Bloom Filter 和 footer；读路径会先查 MemTable/Frozen，再查 SST，SST 上用 Bloom Filter 快速排除不存在的 key，用 BlockCache 缓存热点 block。

在 WiscKey 方面，我把大 value 分离到 VLog。是否分离由 `WISCKEY_VALUE_THRESHOLD` 控制，当前配置为 12；需要注意的是 12 字节本身也是 ValuePointer 的长度，即 8 字节 offset 加 4 字节 value size。自动提交 put 会先把大 value 追加到 VLog，得到引用后再写 WAL，最后写 MemTable。显式事务也是在 put 阶段完成 VLog 转换，commit 阶段不再重新转换。Compaction 构建新 SST 时直接搬运现有 value 或 ValuePointer，不重新把 value 追加到 VLog；VLog GC 当前还没有实现。

事务上，我实现了教学型 RU、RC、RR 三条路径。事务上下文里有临时写集 `temp_map_`、RU 回滚区 `rollback_map_`、RR 读缓存 `read_map_` 和 WAL Record 列表 `operations`。RU 的写会立即进入 MemTable，所以它需要在 put 阶段做写冲突检测，并把旧值记录到 rollback_map，abort 时按旧值回滚。RC 和 RR 的写先放到 temp_map，commit 时统一检测写冲突，成功后先写 WAL，再写 MemTable。RR 相比 RC 多了 read_map，同一个 key 第一次读到什么，后续就返回缓存结果。

事务 id 的推进分为 active、ready 和 flushed。显式事务创建后进入 `activeTrans_`；commit 或 abort 后进入 `readyToFlushTrancIds_`，表示事务已经结束但还没确认刷入 SST；Flush 构建 SST 时收集事务结束标记，把已刷入事务交给 `add_flushed_tranc_ids`，推进 checkpoint。WAL cleaner 使用这个 checkpoint 扫描旧 WAL 文件，文件中所有 record 的事务 id 都不大于 checkpoint 时才能删除。恢复时会读取 checkpoint 之后的 WAL Record，只重放出现 commit 的事务。

Compaction 当前是后台触发的简化 leveled full compaction。L0 超过阈值后，把 L0 全部 SST 和 L1 全部 SST 合并；L0 因为 key range 可能重叠，所以先用堆归并，L1 以上用 ConcactIterator 顺序遍历，再通过 TwoMergeIterator 合并。生成新 SST 时会根据最老活跃事务 id 过滤版本：同 key 下保留所有大于 watermark 的版本，再保留第一个小于等于 watermark 的版本；空 value 的 Tombstone 也按这个规则处理。

上层 RedisWrapper 兼容了部分 Redis RESP 命令，把 String、Hash、Set、List、ZSet 编码成内部 KV。Hash field、Set member、ZSet elem/score 都通过前缀 key 组织，同时用 `REDIS_TYPE_ + key` 做统一命名空间类型检查。为了优化热点复杂数据结构操作，我做了进程内懒加载缓存：Hash 缓存 field 集合和 size，Set 缓存 member 集合和 size，ZSet 缓存 elem->score 和 size。第一次访问通过 LSM 前缀扫描构建缓存，后续写入增量维护；不过当前缓存不是持久化的，进程重启后会重新懒加载，Set 的 `SREM` 路径也还没有完整同步维护成员缓存，这点面试时要如实说明。
