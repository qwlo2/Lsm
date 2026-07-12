# 项目面试总索引

## 1. 两个项目的一句话定位

**TinyLSM：** 基于 C++20 的单机持久化 KV 存储系统，底层实现 LSM-Tree、WAL、SST、Compaction、MVCC 和 WiscKey，上层扩展部分 Redis RESP 命令与复杂数据结构。

**TingyWeb：** 基于 C++14 的轻量级 HTTP Web Server，使用非阻塞 socket、epoll、单 Reactor、线程池、Buffer、mmap/writev 和小根堆定时器实现静态资源服务与登录注册示例。

## 2. 两个项目的 30 秒介绍

### TinyLSM 30 秒

TinyLSM 是一个 C++20 实现的单机持久化 KV 存储系统，底层是 LSM-Tree：写入先进入 WAL，再写 SkipList MemTable，MemTable 满后转成 Frozen MemTable 并刷成 SSTable，后台 Compaction 合并多层 SST。上层实现了部分 Redis RESP 命令，把 String、Hash、Set、List、ZSet 编码成内部 KV，并为 Hash/Set/ZSet 做了进程内成员索引缓存。项目用 GoogleTest 覆盖 WAL、恢复、Compaction、WiscKey 和 RedisWrapper；在 VMware、100 并发、10 万请求下热点 GET 约 20.3 万 QPS，百万随机空间下 SET/HSET/SADD/ZADD 约 5.7/5.1/5.2/2.5 万 QPS。

### TingyWeb 30 秒

TingyWeb 是一个 C++14 实现的轻量级 HTTP Web Server，核心是 epoll + 非阻塞 socket + 单 Reactor 事件循环。主线程负责 accept 和 epoll 事件分发，读写事件通过线程池处理；HTTP 层用 Buffer 解析请求，支持 GET 静态资源、POST 表单登录注册、Keep-Alive；响应侧用 mmap 映射静态文件，再通过 writev 一次发送响应头和文件内容，同时用小根堆定时器清理空闲连接。用户 wrk 记录显示，在 8 vCPU、4GB Ubuntu 虚拟机、同机压测、2 线程、10000 并发连接下，平均吞吐约 1.46 万 QPS。

## 3. TinyLSM 最值得讲的 5 个点

1. **WAL-first 写入和恢复：** `LSM::put/put_batch` 先写 `TranManager::write_to_wal` 和 `WAL::log`，再写 MemTable；重启时 `WAL::recover` 只重放 checkpoint 后已提交事务。
2. **LSM 读写闭环：** SkipList MemTable、Frozen MemTable、SST、Block、Bloom Filter、BlockCache、Flush 和 Compaction 串成完整读写链路。
3. **Redis 复杂结构 KV 编码：** 用类型 key 保证统一命名空间，用前缀 key 表达 Hash field、Set member、ZSet score/member。
4. **成员索引缓存：** Hash/Set/ZSet 首次访问通过前缀扫描懒加载，后续写入删除增量维护 size、member、member-score。
5. **MVCC 与 WiscKey 扩展：** Compaction 根据活跃事务 watermark 保留必要版本和 Tombstone；大 value 通过 VLog 引用减少 Compaction 重写。

## 4. TingyWeb 最值得讲的 5 个点

1. **单 Reactor + 线程池：** `WebServer::Start` 主线程 epoll/accept/分发，`ThreadPool::AddTask` 把读写任务交给工作线程。
2. **非阻塞 I/O 与 EPOLLONESHOT：** fd 设置非阻塞，连接事件启用 `EPOLLONESHOT`，处理后 `ModFd` 重新注册，避免同一连接被多线程同时处理。
3. **Buffer/readv：** `Buffer::ReadFd` 用两个 iovec 把数据读到内部缓冲区和额外栈缓冲区，减少多次 read 和扩容。
4. **mmap/writev 静态响应：** `HttpResponse::AddContent_` mmap 文件，`HttpConn::write` 用 writev 聚合响应头和文件。
5. **小根堆定时器：** `HeapTimer` 用 fd->index ref map 支持 add/adjust/tick，配合 epoll_wait timeout 清理空闲连接。

## 5. 面试时按岗位调整重点

### C++ 后端岗位

优先讲：

1. TingyWeb 的 epoll、非阻塞、线程池、Buffer、mmap/writev。
2. TinyLSM 的 RedisWrapper、RESP Server、缓存、锁和 benchmark。
3. 两个项目中的 RAII、智能指针、线程同步、错误处理边界。

注意口径：

- 不要把 TingyWeb 说成生产 Web Server。
- 不要把 TinyLSM 说成完整 Redis。

### 数据库内核/存储引擎岗位

优先讲：

1. TinyLSM 的 WAL、MemTable、SST、Block、Bloom、BlockCache。
2. Flush、Compaction、Tombstone、MVCC watermark、WiscKey/VLog。
3. RedisWrapper 作为上层 workload 如何映射到底层 LSM。

少讲：

- TingyWeb 只作为网络编程补充，不要喧宾夺主。

### 基础架构岗位

优先讲：

1. TinyLSM 的后台 flush/compact、恢复、缓存和性能瓶颈分析。
2. TingyWeb 的事件驱动、线程池、定时器、同机压测结果。
3. 两个项目在高并发/高吞吐下的瓶颈定位与改进方案。

## 6. C++ 后端岗位讲述顺序

1. 先讲 TingyWeb：从 socket 到 epoll，到线程池，到 HTTP parser，到 mmap/writev。
2. 再讲 TinyLSM：从 Redis 命令到 RedisWrapper，再到 LSM 写入和读路径。
3. 最后对比两个项目的并发模型：TingyWeb 面向网络 I/O，TinyLSM 面向存储 I/O 和后台任务。

推荐开场：

> 我有两个偏系统方向的 C++ 项目：一个是 epoll + 线程池的 HTTP Server，主要体现网络 I/O 和并发处理；另一个是 LSM-Tree KV 存储引擎，上层接了部分 Redis 协议，主要体现存储引擎、WAL、Compaction 和数据结构映射。

## 7. 数据库岗位讲述顺序

1. 先讲 TinyLSM 的整体架构：RESP -> RedisWrapper -> LSM -> WAL/MemTable/SST。
2. 展开写路径：WAL-first、MemTable、Frozen、Flush。
3. 展开读路径：MemTable、L0/L1+、Bloom、BlockCache、WiscKey resolve。
4. 展开 Compaction：迭代器归并、Tombstone、MVCC watermark。
5. 最后讲 RedisWrapper 如何带来真实 workload 和性能结果。

推荐开场：

> 我更想重点讲 TinyLSM，因为它覆盖了存储引擎核心链路：WAL、MemTable、SST、Compaction、MVCC、WiscKey，以及 Redis 数据结构到底层 KV 的映射。

## 8. 网络服务器岗位讲述顺序

1. 先讲 TingyWeb 的主循环和 Reactor 模型。
2. 展开连接生命周期：accept、AddClient、EPOLLIN/EPOLLOUT、CloseConn。
3. 展开 I/O 优化：readv、mmap、writev、Keep-Alive。
4. 展开定时器和线程池。
5. 最后讲 wrk 数据和当前瓶颈。

推荐开场：

> TingyWeb 是我用来系统练习 Linux 网络编程的项目，核心不是业务页面，而是非阻塞 I/O、epoll、线程池、HTTP 请求解析、静态文件响应和连接超时管理。

## 9. 两个项目之间如何自然衔接

### 从 TingyWeb 转到 TinyLSM

> TingyWeb 主要解决网络层的并发连接和 HTTP 请求处理；TinyLSM 则更偏数据层，解决写入持久化、崩溃恢复、LSM Compaction 和 Redis 数据结构映射。两个项目刚好覆盖了后端系统中“请求如何进来”和“数据如何可靠存储”两个方向。

### 从 TinyLSM 转到 TingyWeb

> TinyLSM 上层也有一个 RESP Server，但它的网络层是比较简化的 Asio 实现。为了更深入理解网络服务端，我还实现了 TingyWeb，重点练习 epoll、非阻塞 I/O、线程池、mmap/writev 和定时器。

## 10. 快速避坑清单

TinyLSM 不要说：

- 完整兼容 Redis。
- 工业级事务或 Serializable。
- VLog 已实现 GC。
- 每次写都 fsync。
- benchmark 证明所有随机读写都高性能。

TingyWeb 不要说：

- 多 Reactor。
- 完整 HTTP/1.1。
- 超过 Nginx。
- 540ms 是低延迟。
- mmap + writev 是严格全链路零拷贝。
- MySQL 登录注册已经安全防注入。

## 11. 文档入口

- TinyLSM 完整面试材料：`docs/tinylsm_interview.md`
- TingyWeb 完整面试材料：`docs/tingyweb_interview.md`
- 当前总索引：`docs/project_interview_index.md`

